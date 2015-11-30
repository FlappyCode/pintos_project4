#include <string.h>
#include <stdbool.h>
#include <debug.h>
#include "threads/synch.h"
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
};

#define CACHE_SIZE 64
struct cache_entry cache[CACHE_SIZE];

struct lock cache_lock;

int hand;

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
  	ce->accessed = false;
  	ce->dirty = false;
  	ce->has_data = false;
  	ce->waiters = 0;
  }

  lock_init (&cache_lock);
  hand = -1;
}

struct cache_entry* 
cache_alloc_and_lock (block_sector_t sector, bool exclusive)
{	
	struct cache_entry *ce;
	int i;

begin:
	lock_acquire (&cache_lock);

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

		lock_release (&cache_lock);

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

		lock_release (&cache_lock);

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
	}
	else if (!ce->has_data) 
  {	
  	block_read (fs_device, ce->sector, ce->data);
  	ce->dirty = false; 
  }

  ce->has_data = true;
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
