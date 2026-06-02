#include "pipe.h"
#include "kheap.h"
#include "klog.h"
#include "spinlock.h"
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static inline uint32_t pipe_available(pipe_t* p)
{
    return p->count;
}

static inline uint32_t pipe_space(pipe_t* p)
{
    return PIPE_BUF_SIZE - p->count;
}

// ---------------------------------------------------------------------------
// VFS vtable — read side
// ---------------------------------------------------------------------------

static int pipe_read(vfs_file_t* file, void* buf, uint32_t len)
{
    pipe_t* p = (pipe_t*)file->fs_data;
    if (!p || len == 0)
        return 0;

    uint8_t* dst = (uint8_t*)buf;
    uint32_t read = 0;

    while (read < len) {
        // Block while buffer is empty and writer is still alive.
        // hlt wakes on every IRQ; re-check the condition each time.
        // IF must be 1 here — the syscall handler enables interrupts before
        // calling vfs_read for fd==0 (stdin); SYS_PIPE fds follow the same
        // pattern because the caller is expected to sti before blocking reads.
        while (1) {
            spinlock_acquire(&p->lock);
            uint32_t avail = pipe_available(p);
            spinlock_release(&p->lock);

            if (avail > 0)
                break;

            if (!p->write_open)
                return (int)read;   // EOF — all write ends closed, nothing left

            __asm__ volatile ("hlt");
        }

        spinlock_acquire(&p->lock);

        uint32_t avail = pipe_available(p);
        uint32_t take  = (len - read) < avail ? (len - read) : avail;

        for (uint32_t i = 0; i < take; i++) {
            dst[read++] = p->buf[p->read_pos];
            p->read_pos = (p->read_pos + 1) & (PIPE_BUF_SIZE - 1);
        }
        p->count -= take;

        spinlock_release(&p->lock);

        // Return as soon as we have any data — don't force callers to wait
        // for `len` bytes.  Matches POSIX read() semantics.
        break;
    }

    return (int)read;
}

// ---------------------------------------------------------------------------
// VFS vtable — write side
// ---------------------------------------------------------------------------

static int pipe_write(vfs_file_t* file, const void* buf, uint32_t len)
{
    pipe_t* p = (pipe_t*)file->fs_data;
    if (!p || len == 0)
        return 0;

    // Broken pipe: all readers already closed.
    if (!p->read_open)
        return -1;

    const uint8_t* src     = (const uint8_t*)buf;
    uint32_t       written = 0;

    while (written < len) {
        // Block while buffer is full.
        while (1) {
            spinlock_acquire(&p->lock);
            uint32_t space = pipe_space(p);
            spinlock_release(&p->lock);

            if (space > 0)
                break;

            if (!p->read_open)
                return (int)written;  // reader gone mid-write

            __asm__ volatile ("hlt");
        }

        spinlock_acquire(&p->lock);

        uint32_t space = pipe_space(p);
        uint32_t put   = (len - written) < space ? (len - written) : space;

        for (uint32_t i = 0; i < put; i++) {
            p->buf[p->write_pos] = src[written++];
            p->write_pos = (p->write_pos + 1) & (PIPE_BUF_SIZE - 1);
        }
        p->count += put;

        spinlock_release(&p->lock);
    }

    return (int)written;
}

// ---------------------------------------------------------------------------
// VFS vtable — close (shared by both sides, direction from file->flags)
// ---------------------------------------------------------------------------

static int pipe_close(vfs_file_t* file)
{
    pipe_t* p = (pipe_t*)file->fs_data;
    if (!p)
        return 0;

    // Decrement the appropriate reference count under the lock.
    // The free decision must also be made under the lock so no two concurrent
    // closers can both observe counts==0 and both try to free.
    spinlock_acquire(&p->lock);

    if (file->flags & O_RDONLY) {
        if (p->read_open > 0)
            p->read_open--;
    }
    if (file->flags & O_WRONLY) {
        if (p->write_open > 0)
            p->write_open--;
    }

    int should_free = (!p->read_open && !p->write_open);

    spinlock_release(&p->lock);

    if (should_free) {
        klog_info("pipe: both ends closed, freeing pipe_t");
        kfree(p);
        // Null out fs_data so a double-close on this vfs_file_t is safe.
        file->fs_data = NULL;
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Stubs for unused vtable slots
// ---------------------------------------------------------------------------

static int pipe_open(const char* path, uint32_t flags, vfs_file_t* out)
{
    (void)path; (void)flags; (void)out;
    return -1;  // pipes are created via SYS_PIPE, not open()
}

static int pipe_unlink(const char* path)   { (void)path; return -1; }
static int pipe_readdir(vfs_file_t* dir, uint32_t idx, vfs_dirent_t* out)
    { (void)dir; (void)idx; (void)out; return -1; }
static int pipe_mkdir(const char* path)    { (void)path; return -1; }
static int pipe_rmdir(const char* path)    { (void)path; return -1; }
static int pipe_stub_create(const char* path) { (void)path; return -1; }

// Two vtables: one for each end.  Using separate vtables means vfs_read /
// vfs_write can check file->flags without a special-case in the pipe driver,
// and it's clear at a glance which side does what.

static vfs_ops_t pipe_read_ops = {
    .open    = pipe_open,
    .read    = pipe_read,
    .write   = pipe_write,   // writing to read-end is an error at vfs layer
    .close   = pipe_close,
    .unlink  = pipe_unlink,
    .readdir = pipe_readdir,
    .mkdir   = pipe_mkdir,
    .rmdir   = pipe_rmdir,
    .create  = pipe_stub_create,
};

static vfs_ops_t pipe_write_ops = {
    .open    = pipe_open,
    .read    = pipe_read,    // reading from write-end is an error at vfs layer
    .write   = pipe_write,
    .close   = pipe_close,
    .unlink  = pipe_unlink,
    .readdir = pipe_readdir,
    .mkdir   = pipe_mkdir,
    .rmdir   = pipe_rmdir,
    .create  = pipe_stub_create,
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int pipe_create(vfs_file_t** read_file, vfs_file_t** write_file)
{
    pipe_t* p = (pipe_t*)kmalloc(sizeof(pipe_t));
    if (!p)
        return -1;

    // Zero-initialise the struct.
    uint8_t* pb = (uint8_t*)p;
    for (uint32_t i = 0; i < sizeof(pipe_t); i++)
        pb[i] = 0;

    // Both counts start at 1 — one holder of each end.
    p->read_open  = 1;
    p->write_open = 1;

    // Allocate the read-side vfs_file_t.
    vfs_file_t* rf = (vfs_file_t*)kmalloc(sizeof(vfs_file_t));
    if (!rf) {
        kfree(p);
        return -1;
    }

    // Allocate the write-side vfs_file_t.
    vfs_file_t* wf = (vfs_file_t*)kmalloc(sizeof(vfs_file_t));
    if (!wf) {
        kfree(rf);
        kfree(p);
        return -1;
    }

    // Zero both file structs.
    uint8_t* rfb = (uint8_t*)rf;
    uint8_t* wfb = (uint8_t*)wf;
    for (uint32_t i = 0; i < sizeof(vfs_file_t); i++)
        rfb[i] = wfb[i] = 0;

    rf->flags    = O_RDONLY;
    rf->fs_data  = p;
    rf->ops      = &pipe_read_ops;

    wf->flags    = O_WRONLY;
    wf->fs_data  = p;
    wf->ops      = &pipe_write_ops;

    *read_file  = rf;
    *write_file = wf;

    klog_info("pipe: created pipe_t=%x", (uint32_t)p);
    return 0;
}

void pipe_ref_read(pipe_t* p)
{
    spinlock_acquire(&p->lock);
    p->read_open++;
    spinlock_release(&p->lock);
}

void pipe_ref_write(pipe_t* p)
{
    spinlock_acquire(&p->lock);
    p->write_open++;
    spinlock_release(&p->lock);
}