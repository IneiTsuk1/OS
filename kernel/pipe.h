#pragma once
#include <stdint.h>
#include "vfs/vfs.h"

// ---------------------------------------------------------------------------
// Kernel pipe — anonymous byte stream connecting two file descriptors.
//
// A pipe_t is a fixed-size circular buffer in kernel memory shared between
// a read end and a write end.  Both ends are represented as ordinary
// vfs_file_t entries in a task's fd table, backed by the pipe_ops vtable.
//
// Lifetime:
//   pipe_create() allocates one pipe_t and returns two open vfs_file_t*
//   (read side, write side).  Each side holds a pointer to the shared
//   pipe_t in its fs_data field.
//
//   read_open / write_open are reference counts, not binary flags.
//   pipe_ref_read() / pipe_ref_write() increment the appropriate count
//   when a shallow copy of a vfs_file_t is made (e.g. in SYS_EXECP).
//   When a side is closed (pipe_ops.close), the corresponding count is
//   decremented.  When both counts reach zero the pipe_t is freed.
//
// Blocking:
//   Reads block (hlt loop) when the buffer is empty and write_open > 0.
//   An empty buffer with write_open==0 returns 0 (EOF).
//   Writes block when the buffer is full.  With interrupts enabled the hlt
//   wakes on each PIT/keyboard IRQ and re-checks the condition.
//
// Thread safety:
//   A spinlock inside pipe_t serialises concurrent read/write/close from
//   different tasks.  The free decision (both counts == 0) is made under
//   the lock so no two closers can both decide to free.
// ---------------------------------------------------------------------------

#define PIPE_BUF_SIZE   4096    // power of two — enables cheap modulo masking

#include "spinlock.h"

typedef struct pipe {
    uint8_t      buf[PIPE_BUF_SIZE];   // circular data buffer
    uint32_t     read_pos;             // index of next byte to read
    uint32_t     write_pos;            // index of next byte to write
    uint32_t     count;                // bytes currently in buffer
    uint8_t      read_open;            // reference count of open read ends
    uint8_t      write_open;           // reference count of open write ends
    spinlock_t   lock;                 // serialises count / pos / refcount updates
} pipe_t;

// Allocate a pipe and open both ends.
//
// On success:
//   *read_file  receives a kmalloc'd vfs_file_t for the read side  (O_RDONLY)
//   *write_file receives a kmalloc'd vfs_file_t for the write side (O_WRONLY)
//   Both counts start at 1.
//   Returns 0.
//
// On failure (OOM):
//   Returns -1.  Neither file is allocated.
int pipe_create(vfs_file_t** read_file, vfs_file_t** write_file);

// Increment the read-end reference count.
// Call after making a shallow copy of a read-side vfs_file_t (e.g. SYS_EXECP).
void pipe_ref_read(pipe_t* p);

// Increment the write-end reference count.
// Call after making a shallow copy of a write-side vfs_file_t (e.g. SYS_EXECP).
void pipe_ref_write(pipe_t* p);