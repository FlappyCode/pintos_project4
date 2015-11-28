#include <string.h>
#include "devices/timer.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/syscall.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "filesys/file.h"
#include "vm/frame.h"
#include "vm/page.h"
#include "vm/swap.h"

/* The frame table. */
struct list frame_table;

/* Global lock for frame table. 
   Used when modifying frame table. */
struct lock frame_table_lock; 

/* Init frame table .*/
void
frame_table_init (void)
{
	list_init (&frame_table);
	lock_init (&frame_table_lock);
}

/* Try to allocate a frame for page. */
/* Immediately lock it after allocation. */
/* If no frame avaliable, try to evict one .*/
/* Return NULL on failure. */
struct frame_table_entry *
frame_alloc_and_lock (struct spt_entry *spe)
{
	/* Since we will modify the frame table, 
     acquire global lock first. */ 
  lock_acquire (&frame_table_lock);

  /* First try to get free frame. */
	uint8_t* k_addr = palloc_get_page (PAL_USER | PAL_ZERO);
	struct frame_table_entry *fte = NULL;

	if (k_addr && (fte = malloc (sizeof (struct frame_table_entry))))
	{	
		/* If we have free frame, return one. */
    fte->k_addr = k_addr;
		fte->spe = spe;
		lock_init (&fte->l);
		lock_acquire (&fte->l);
		list_push_back (&frame_table, &fte->elem);
		lock_release (&frame_table_lock);
		return fte;
	} 
	else
	{	
	  /* If we don't have free frame, try to evict. */
	  ASSERT (!list_empty (&frame_table));
  	struct list_elem *e = list_begin(&frame_table);
  	struct spt_entry *spe_tmp = NULL;

    /* Each time we scan frame table table.
       At most try 3 scans. */
    int try_num = 2*3;
  	while (try_num) 
    {
      /* Get a frame from the frame table. */
      fte = list_entry (e, struct frame_table_entry, elem);

      /* Must lock the frame first to prevent race. */
      /* If failed, other page is modifying it, we 
         continue to find next frame. */
      if (lock_try_acquire (&fte->l))
      {
      	spe_tmp = fte->spe;

        /* clock algorithm. */
      	if (pagedir_is_accessed (spe_tmp->t->pagedir, spe_tmp->u_addr))
      	{
    			/* If it has been accessed recently, clear the access bit. */
          pagedir_set_accessed (spe_tmp->t->pagedir, spe_tmp->u_addr, false);
    			lock_release (&fte->l);
    		}
    		else
    		{	
    			/* Try to evict this frame. */
          bool success;
          block_sector_t sector_id = (block_sector_t) -1;
          
    			/* Must first set the page to be not present in page table
             before checking the dirty bit.
             This will prevent a race that another process is dirtying the
             process. After setting not present, other processes wanting
             to dirty this page will fault and load again. When they try to 
             load again, since they can't get the frame lock, they must wait 
             for this process to end evicting, thus preventing the race. */
    			pagedir_clear_page(spe_tmp->t->pagedir, spe_tmp->u_addr);
    			
  				/* Write frame back to file/swap if necessary. */
  				if (spe_tmp->file != NULL) 
    			{
      			/* Check dirty bit. */
            if (pagedir_is_dirty (spe_tmp->t->pagedir, spe_tmp->u_addr)) 
        		{
          		if (spe_tmp->type == VM_EXECUTABLE_TYPE)
              {
          			/* Modified excutable file page will be written to swap. */
                success = (sector_id = swap_alloc (fte->k_addr)) != (block_sector_t) -1;
          		}
              else
          		{
          		  /* Modified mmap file page will be written to file. */
                /* Use try acquire lock to avoid dead lock. */
                if(try_acquire_file_lock ())
                {
                  success = file_write_at (spe_tmp->file, fte->k_addr, spe_tmp->file_bytes,
                    spe_tmp->ofs) == (int) spe_tmp->file_bytes;
                  release_file_lock ();
                }
                else
                { 
                  /* Can't get the lock, skip this frame to try to evict another. */
                  success = false;
                }
          		}
          	}
          	else
            {
          		/* Clean page, return directly. */
              success = true;
            }
          }
          else
          {
          	/* Stack page, write it to swap. */
            success = (sector_id = swap_alloc (fte->k_addr)) != (block_sector_t) -1;
          }

          if (success) 
    			{
      			/* Evict successful. */
            spe_tmp->fte = NULL;
      			spe_tmp->sector_id = sector_id;
      			fte->spe = spe;
      			if (spe->file != NULL)
      				memset (fte->k_addr + spe->file_bytes, 0, PGSIZE - spe->file_bytes);
      			lock_release (&frame_table_lock);
      			return fte;
    			}
    			else
    			{
    				/* Can't evict a frame, sleep and try again. */
            lock_release (&fte->l);
            try_num--;
            if (try_num > 0 && (try_num % 2 == 0))
              timer_msleep (100);
    			}
    		}
      
    	}
    	e = list_next (e);
    	if (e == list_end (&frame_table))
    		e = list_begin (&frame_table);
    }
	}

	return NULL;
}

/* Free a frame. */
/* Must hold this frame's lock before calling this function. */
void 
frame_release_and_free (struct frame_table_entry *fte)
{	
  lock_acquire (&frame_table_lock);
  list_remove (&fte->elem);
  pagedir_clear_page(fte->spe->t->pagedir, fte->spe->u_addr);
  palloc_free_page (fte->k_addr);
  lock_release (&frame_table_lock);

  lock_release (&fte->l);
  free (fte);
  return;
}
