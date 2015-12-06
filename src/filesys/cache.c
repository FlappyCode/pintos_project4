#include <string.h>
#include <stdbool.h>
#include <debug.h>
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "devices/timer.h"
#include "filesys/filesys.h"
#include "filesys/cache.h"

struct cache_entry
{
	block_sector_t sector;
	bool accessed;
	bool dirty;
	bool has_data;
	int waiters;
	uint8_t data[BLOCK_SECTOR_SIZE];
	struct lock l;
	struct shared_lock sl;
	struct lock has_data_lock;
};

#define CACHE_SIZE 64
struct cache_entry cache[CACHE_SIZE];

/* Protect clock hand .*/
struct lock cache_lock;
int hand;

struct readahead_s
{
	struct list_elem elem;
	block_sector_t sector;
};

static struct list readahead_list;
static struct lock readahead_lock;
static struct condition need_readahead;

static void cache_readahead_daemon (void *aux UNUSED);
static void cache_flush_daemon (void *aux UNUSED);

void cache_init (void)
{	
	struct cache_entry *ce;
	int i;
  for (i = 0; i < CACHE_SIZE; i++) 
  {
  	ce = &cache[i];
  	ce->sector = (block_sector_t) -1;
  	lock_init (&ce->l);
  	shared_lock_init (&ce->sl, &ce->l);
  	lock_init (&ce->has_data_lock);
  	ce->accessed = false;
  	ce->dirty = false;
  	ce->has_data = false;
  	ce->waiters = 0;
  }

  lock_init (&cache_lock);
  hand = -1;

  thread_create ("cache_flush_daemon", PRI_MIN, cache_flush_daemon, NULL);

  list_init (&readahead_list);
  lock_init (&readahead_lock);
  cond_init (&need_readahead);
  thread_create ("cache_readahead_daemon", PRI_MIN, cache_readahead_daemon, NULL);
}

struct cache_entry* 
cache_alloc_and_lock (block_sector_t sector, bool exclusive)
{	
	struct cache_entry *ce;
	int i;

begin:

	/* Sector may have been cached, check it .*/ 
	for (i = 0 ; i < CACHE_SIZE ; i++)
	{	
		ce = &cache[i];
		lock_acquire (&ce->l);
		if (ce->sector != sector)
		{	
			lock_release (&ce->l);
			continue;
		}

		ce->waiters++;
		shared_lock_acquire (&ce->sl, exclusive);
		ce->waiters--;

		ASSERT (ce->sector == sector);

		lock_release (&ce->l);
		return ce;
	}

	/* Try to find an empty entry. */
	for (i = 0 ; i < CACHE_SIZE ; i++)
	{	
		ce = &cache[i];
		lock_acquire (&ce->l);
		if (ce->sector != (block_sector_t) -1)
		{	
			lock_release (&ce->l);
			continue;
		}

		ce->sector = sector;
		ce->accessed = false;
  	ce->dirty = false;
  	ce->has_data = false;
  	ce->waiters = 0;
		ASSERT (shared_lock_try_acquire (&ce->sl, exclusive));
		ASSERT (ce->waiters == 0);
		lock_release (&ce->l);
		return ce;
	}

	lock_acquire (&cache_lock);
	/* Try to evict one entry. */
	for (i = 0; i < CACHE_SIZE * 2; i++)
	{	
		if (++hand >= CACHE_SIZE)
			hand = 0;

		ce = &cache[hand];
		if(!lock_try_acquire (&ce->l))
			continue;
		else if (!shared_lock_try_acquire (&ce->sl, true))
		{	
			lock_release (&ce->l);
			continue;
		}
		else if (ce->waiters != 0)
		{	
			shared_lock_release (&ce->sl, true);
			lock_release (&ce->l);
			continue;
		}
		else if (ce->accessed)
		{	
			ce->accessed = false;
			shared_lock_release (&ce->sl, true);
			lock_release (&ce->l);
			continue;
		}

		lock_release (&cache_lock);
		
		if (ce->has_data && ce->dirty) 
    {	
    	lock_release (&ce->l);
    	block_write (fs_device, ce->sector, ce->data);
    	ce->dirty = false;
    	lock_acquire (&ce->l);
    }

    if (ce->waiters == 0)
    {	
    	ce->sector = (block_sector_t) -1;
    }

    shared_lock_release (&ce->sl, true);
    lock_release (&ce->l);

    goto begin;
  }

  lock_release (&cache_lock);
  timer_msleep (100);
  goto begin;
}

void 
cache_unlock (struct cache_entry* ce, bool exclusive)
{	
	lock_acquire (&ce->l);
	shared_lock_release (&ce->sl, exclusive);
	lock_release (&ce->l);
}

void* 
cache_get_data (struct cache_entry* ce, bool zero)
{	
	if (zero)
	{	
		memset (ce->data, 0, BLOCK_SECTOR_SIZE);
		ce->dirty = true;
		ce->has_data = true;
	}
	else
  {	
  	lock_acquire (&ce->has_data_lock);
  	if (!ce->has_data)
  	{ 
  		block_read (fs_device, ce->sector, ce->data);
  		ce->dirty = false;
  		ce->has_data = true;
  	} 
  	lock_release (&ce->has_data_lock);
  }

  ce->accessed = true;
  return ce->data;
}

void
cache_dealloc (block_sector_t sector) 
{
  int i;
  struct cache_entry *ce;
  
  lock_acquire (&cache_lock);
  for (i = 0; i < CACHE_SIZE; i++)
  {
  	ce = &cache[i];
  	lock_acquire (&ce->l);
  	if (ce->sector == sector) 
  	{
  		lock_release (&cache_lock);
			
			if (shared_lock_try_acquire (&ce->sl, true))
			{	
				if (ce->waiters == 0)
					ce->sector = (block_sector_t) -1;
				shared_lock_release (&ce->sl, true);
			}
			
			lock_release (&ce->l);
			return;
    }
    lock_release (&ce->l);
  }
  lock_release (&cache_lock);
}

void 
cache_mark_dirty (struct cache_entry *ce)
{	
	ASSERT (ce->has_data);
	ce->dirty = true;
}

void
cache_flush (void) 
{
  struct cache_entry *ce;
  block_sector_t sector;
  int i;
  
  for (i = 0; i < CACHE_SIZE; i++)
  {
  	ce = &cache[i];
  	lock_acquire (&ce->l);
    sector = ce->sector;

    if (sector == (block_sector_t) -1)
    {
    	lock_release (&ce->l);
    	continue;
    }

   	lock_release (&ce->l);
    ce = cache_alloc_and_lock (sector, true);
    if (ce->has_data && ce->dirty) 
    {	
    	block_write (fs_device, ce->sector, ce->data);
    	ce->dirty = false; 
    }
    cache_unlock (ce, true);
  }
}

void
cache_readahead_add (block_sector_t sector) 
{
  struct readahead_s *ras = malloc (sizeof *ras);
  if (ras == NULL)
    return;
 	ras->sector = sector;

  lock_acquire (&readahead_lock); 
  list_push_back (&readahead_list, &ras->elem);
  cond_signal (&need_readahead, &readahead_lock);
  lock_release (&readahead_lock);
}

static void 
cache_flush_daemon (void *aux UNUSED)
{	
	while (true)
	{	
		timer_msleep (20 * 1000);
		cache_flush ();
	}
}

static void
cache_readahead_daemon (void *aux UNUSED) 
{
  while (true) 
  {	
    lock_acquire (&readahead_lock);
    while (list_empty (&readahead_list)) 
    	cond_wait (&need_readahead, &readahead_lock);
    
    ASSERT (!list_empty (&readahead_list));
    struct readahead_s *ras = list_entry (list_pop_front (&readahead_list),
                             struct readahead_s, elem);
    lock_release (&readahead_lock);

    struct cache_entry *ce = cache_alloc_and_lock (ras->sector, false);
    cache_get_data (ce, false);
    cache_unlock (ce, false);
    free (ras);
  }
}