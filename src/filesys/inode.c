#include <stdio.h>
#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "threads/synch.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

#define SECTOR_PTR_SIZE (sizeof (block_sector_t))
#define SECTOR_PTR_CNT (BLOCK_SECTOR_SIZE / SECTOR_PTR_SIZE)
#define META_PTR_CNT 3
#define BLOCK_PTR_CNT (SECTOR_PTR_CNT - META_PTR_CNT)
#define INDIRECT_BLOCK_CNT 1
#define DOUBLE_INDIRECT_BLOCK_CNT 1
#define DATA_BLOCK_CNT (BLOCK_PTR_CNT - INDIRECT_BLOCK_CNT - DOUBLE_INDIRECT_BLOCK_CNT)
#define INODE_MAX_LENGTH ((DATA_BLOCK_CNT + SECTOR_PTR_CNT * INDIRECT_BLOCK_CNT + SECTOR_PTR_CNT * SECTOR_PTR_CNT * DOUBLE_INDIRECT_BLOCK_CNT) * BLOCK_SECTOR_SIZE)

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    block_sector_t sectors[BLOCK_PTR_CNT]; /* Sectors. */
    off_t length;                       /* File size in bytes. */
    int type;                           /* File : 0 ; dir : 1 */
    unsigned magic;                     /* Magic number. */
  };

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct lock inode_lock;
  };

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;
static struct lock open_inodes_lock;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
  lock_init (&open_inodes_lock);
}

struct inode *
inode_create (block_sector_t sector, bool is_dir)
{
  struct inode_disk *disk_inode;
  struct cache_entry *ce;

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  ce = cache_alloc_and_lock (sector, true);
  disk_inode = cache_get_data (ce, true);
  disk_inode->length = 0;
  disk_inode->type = is_dir ? 1 : 0; 
  disk_inode->magic = INODE_MAGIC;
  cache_mark_dirty (ce);
  cache_unlock (ce, true);

  struct inode* inode = inode_open (sector);
  if (inode == NULL)
    cache_dealloc (sector);
  return inode;
}

bool inode_is_dir(struct inode *inode)
{
  if (inode == NULL) return false;
  struct cache_entry *ce = cache_alloc_and_lock (inode->sector, false);
  struct inode_disk *disk_inode = cache_get_data (ce, false);
  int type = disk_inode->type;
  cache_unlock (ce, false);
  return (type == 1);
}

int inode_open_cnt(struct inode *inode)
{
  int value;
  lock_acquire(&open_inodes_lock);
  value = inode->open_cnt;
  lock_release(&open_inodes_lock);
  return value;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  lock_acquire (&open_inodes_lock);
  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
  {
    inode = list_entry (e, struct inode, elem);
    if (inode->sector == sector) 
    {
      inode->open_cnt++;
      lock_release (&open_inodes_lock);
      return inode; 
    }
  }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
  {
    lock_release (&open_inodes_lock);
    return NULL;
  }

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  lock_init(&inode->inode_lock);
  lock_release (&open_inodes_lock);
  return inode;
}

void inode_acquire_lock(struct inode *inode)
{
  lock_acquire(&inode->inode_lock);
}

void inode_release_lock(struct inode *inode)
{
  lock_release(&inode->inode_lock);
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
  {
    lock_acquire (&open_inodes_lock);
    inode->open_cnt++;
    lock_release (&open_inodes_lock);
  }
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

static void
remove_inode (struct inode *inode)
{ 
  struct cache_entry *ce = cache_alloc_and_lock (inode->sector, true);
  struct inode_disk *disk_inode = cache_get_data (ce, false);
  int i;
  for (i = 0; i < (int) BLOCK_PTR_CNT; i++)
  {
    block_sector_t sector = disk_inode->sectors[i];
    if (sector != 0) 
    {
      int level = (i >= (int) DATA_BLOCK_CNT) + (i >= (int) (DATA_BLOCK_CNT + INDIRECT_BLOCK_CNT));
      switch (level)
      { 
        case 0 :
        {
          cache_dealloc (sector);
          free_map_release (sector, 1);
          break;
        }
        case 1 :
        {
          struct cache_entry *ce1 = cache_alloc_and_lock (sector, true);
          block_sector_t *disk_inode1 = cache_get_data (ce1, false);
          int j;
          for (j = 0 ; j < (int) SECTOR_PTR_CNT; j++)
          { 
            block_sector_t sector1 = disk_inode1[j];
            if (sector1 != 0)
            {
              cache_dealloc (sector1);
              free_map_release (sector1, 1);
            }
          }
          cache_unlock (ce1, true);
          cache_dealloc (sector);
          free_map_release (sector, 1);
          break;
        }
        case 2 :
        {
          struct cache_entry *ce2 = cache_alloc_and_lock (sector, true);
          block_sector_t *disk_inode2 = cache_get_data (ce2, false);
          int m;
          for (m = 0 ; m < (int) SECTOR_PTR_CNT; m++)
          { 
            block_sector_t sector2 = disk_inode2[m];
            if (sector2 != 0)
            { 
              struct cache_entry *ce3 = cache_alloc_and_lock (sector2, true);
              block_sector_t *disk_inode3 = cache_get_data (ce3, false);
              int n;
              for (n = 0 ; n < (int) SECTOR_PTR_CNT ; n++)
              {
                block_sector_t sector3 = disk_inode3[n];
                if (sector3 != 0)
                {
                  cache_dealloc (sector3);
                  free_map_release (sector3, 1);
                }
              }
              cache_unlock (ce3, true);
              cache_dealloc (sector2);
              free_map_release (sector2, 1);
            }
          }
          cache_unlock (ce2, true);
          cache_dealloc (sector);
          free_map_release (sector, 1);
          break;
        }
      }
    }
  }

  cache_unlock (ce, true);
  cache_dealloc (inode->sector);
  free_map_release (inode->sector,1);
  return;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  lock_acquire (&open_inodes_lock);
  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
  {
    /* Remove from inode list and release lock. */
    list_remove (&inode->elem);
    lock_release (&open_inodes_lock);
 
    /* Deallocate blocks if removed. */
    if (inode->removed) 
      remove_inode (inode);
      
    free (inode); 
  }
  else
    lock_release (&open_inodes_lock);
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Retrieves the data block for the given byte OFFSET in INODE,
   setting *DATA_BLOCK to the block.
   Returns true if successful, false on failure.
   If ALLOCATE is false, then missing blocks will be successful
   with *DATA_BLOCk set to a null pointer.
   If ALLOCATE is true, then missing blocks will be allocated.
   The block returned will be locked, normally non-exclusively,
   but a newly allocated block will have an exclusive lock. */
static bool
read_block (struct inode *inode, off_t offset, bool is_write, struct cache_entry **ce_result) 
{
  ASSERT (inode != NULL);
  ASSERT (offset >= 0);
  ASSERT (offset <= (off_t) INODE_MAX_LENGTH);

  off_t sector_offs[3];
  int level;
  
  off_t sector_off = offset / BLOCK_SECTOR_SIZE;
  
  if (sector_off < (off_t) DATA_BLOCK_CNT) 
  { 
    sector_offs[0] = sector_off;
    level = 1;
  }
  else
  { 
    sector_off -= DATA_BLOCK_CNT;
    if (sector_off < (off_t) (SECTOR_PTR_CNT * INDIRECT_BLOCK_CNT))
    {
      sector_offs[0] = DATA_BLOCK_CNT + sector_off / SECTOR_PTR_CNT;
      sector_offs[1] = sector_off % SECTOR_PTR_CNT;
      level = 2;
    }
    else
    {
      sector_off -= SECTOR_PTR_CNT * INDIRECT_BLOCK_CNT;
      sector_offs[0] = DATA_BLOCK_CNT + INDIRECT_BLOCK_CNT + sector_off / (SECTOR_PTR_CNT * SECTOR_PTR_CNT);
      sector_offs[1] = sector_off / SECTOR_PTR_CNT;
      sector_offs[2] = sector_off % SECTOR_PTR_CNT;
      level = 3;
    }
  }    

  int this_level = 0;
  block_sector_t sector = inode->sector;
  struct cache_entry *ce;
  uint32_t *data;
  block_sector_t* next_sector;
  struct cache_entry *next_ce;
  while (1) 
  {
    ce = cache_alloc_and_lock (sector, false);
    data = cache_get_data (ce, false);
    next_sector = &data[sector_offs[this_level]];
    if (*next_sector != 0)
    {
      if (this_level == level-1) 
      {
        cache_unlock (ce, false);
        *ce_result = cache_alloc_and_lock (*next_sector, is_write);
        return true;
      }
      
      sector = *next_sector;
      cache_unlock (ce, false);
      this_level++;
      continue;
    }
    
    cache_unlock (ce, false);

    if (!is_write) 
    {
      *ce_result = NULL;
      return true;
    }

    ce = cache_alloc_and_lock (sector, true);
    data = cache_get_data (ce, false);

    next_sector = &data[sector_offs[this_level]];
    if (*next_sector != 0)
    { 
      sector = *next_sector;
      cache_unlock (ce, true);
      this_level++;
      continue;
    }

    if (!free_map_allocate (1, next_sector))
    {
      cache_unlock (ce, true);
      *ce_result = NULL;
      return false;
    }

    cache_mark_dirty (ce);

    next_ce = cache_alloc_and_lock (*next_sector, true);
    cache_get_data (next_ce, true);

    cache_unlock (ce, true);

    if (this_level == level-1) 
    {
      *ce_result = next_ce;
      return true;
    }

    sector = *next_sector;
    cache_unlock (next_ce, true);
    this_level++;
  }
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  ASSERT (inode != NULL);
  ASSERT (offset >= 0);
  ASSERT (size >= 0);
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;

  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      struct cache_entry *ce;
      if (!read_block (inode, offset, false, &ce))
        break;

      if (ce == NULL)
        memset (buffer + bytes_read, 0, chunk_size);
      else
      {
        uint8_t *data = cache_get_data (ce, false);
        memcpy (buffer + bytes_read, data + sector_ofs, chunk_size);
        cache_unlock (ce, false);
      }
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  ASSERT (inode != NULL);
  ASSERT (offset >= 0);
  ASSERT (size >= 0);
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;

  if (inode->deny_write_cnt)
    return 0;

  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = (off_t) INODE_MAX_LENGTH - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      struct cache_entry *ce;
      if (!read_block (inode, offset, true, &ce))
        break;

      uint8_t *data = cache_get_data (ce, false);
      memcpy (data + sector_ofs, buffer + bytes_written, chunk_size);
      cache_mark_dirty (ce);
      cache_unlock (ce, true);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }

  /* Extend File. */
  struct cache_entry *ce1 = cache_alloc_and_lock (inode->sector, true);
  struct inode_disk *disk_inode1 = cache_get_data (ce1, false);
  if (offset > disk_inode1->length) 
  {
    disk_inode1->length = offset;
    cache_mark_dirty (ce1);
  }
  cache_unlock (ce1, true);

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  struct cache_entry *ce = cache_alloc_and_lock (inode->sector, false);
  struct inode_disk *disk_inode = cache_get_data (ce, false);
  off_t length = disk_inode->length;
  cache_unlock (ce, false);
  return length;
}
