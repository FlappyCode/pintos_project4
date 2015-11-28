#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include <stdbool.h>
#include <list.h>

/* Process identifier. */
typedef int pid_t;
typedef int mapid_t;
#define PID_ERROR ((pid_t) -1)

#define EXIT_ERROR -1

/* Data structure used for mmap. */
struct mmap_file {
	struct list_elem elem; /* elem used for list. */
	mapid_t mapid; /* Mmap id. */
	struct spt_entry *spe; /* Corresponding supplemental page table entry. */
};

void syscall_init (void);

void acquire_file_lock (void);
bool try_acquire_file_lock (void);
void release_file_lock (void);

void halt (void);
void exit (int status);
pid_t exec (const char *file);
int wait (pid_t);
bool create (const char *file, unsigned initial_size);
bool remove (const char *file);
int open (const char *file);
int filesize (int fd);
int read (int fd, void *buffer, unsigned length);
int write (int fd, const void *buffer, unsigned length);
void seek (int fd, unsigned position);
unsigned tell (int fd);
void close (int fd);
void close_all (void);
mapid_t mmap (int fd, void *addr);
void munmap (mapid_t mapping);
void close_all_mmap(void);

#endif /* userprog/syscall.h */
