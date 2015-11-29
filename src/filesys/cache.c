/*
#include "filesys/cache.h"

#define CACHE_SIZE 64

struct cache_entry
{
	block_sector_t sector;
	bool accessed;
	bool dirty;
	uint8_t data[BLOCK_SECTOR_SIZE];
	struct lock l;
};

struct cache_entry cache[CACHE_SIZE];

void cache_init (void)
{	

}
*/