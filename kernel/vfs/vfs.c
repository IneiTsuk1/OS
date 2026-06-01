#include "vfs.h"
#include "../klog.h"
#include "../panic.h"
#include "../scheduler.h"
#include "../task.h"
#include "../kheap.h"
#include "../../drivers/vga/terminal.h"
#include "../../drivers/serial/serial.h"
#include "../../drivers/keyboard/keyboard.h"
#include <stddef.h>

// ---------------------------------------------------------------------------
// VFS — single mount point implementation
// ---------------------------------------------------------------------------

static vfs_ops_t* mounted_fs = NULL;

// ---- terminal pseudo-driver ------------------------------------------------
//
// fds 0/1/2 (stdin/stdout/stderr) are backed by this vtable rather than the
// real filesystem.  Reads from stdin return 0 (no blocking keyboard read yet).
// Writes to stdout/stderr go to the VGA terminal and serial port.

static int term_open(const char* path, uint32_t flags, vfs_file_t* out)
{
    (void)path; (void)flags; (void)out;
    return VFS_EOK;
}

static int term_read(vfs_file_t* file, void* buf, uint32_t len)
{
    // stdin: block until input is available (canonical mode — returns on '\n').
    // keyboard_read requires interrupts enabled; the SYS_READ handler re-enables
    // them before calling vfs_read, so this is safe from syscall context.
    (void)file;
    return (int)keyboard_read((char*)buf, len);
}

static int term_write(vfs_file_t* file, const void* buf, uint32_t len)
{
    (void)file;
    const char* p = (const char*)buf;
    for (uint32_t i = 0; i < len; i++) {
        terminal_putchar(p[i]);
        serial_write_char(p[i]);
    }
    return (int)len;
}

static int term_close(vfs_file_t* file)   { (void)file; return VFS_EOK; }
static int term_unlink(const char* path)  { (void)path; return VFS_EGENERIC; }
static int term_readdir(vfs_file_t* dir, uint32_t index, vfs_dirent_t* out)
    { (void)dir; (void)index; (void)out; return VFS_EGENERIC; }
static int term_mkdir(const char* path)   { (void)path; return VFS_EGENERIC; }
static int term_rmdir(const char* path)   { (void)path; return VFS_EGENERIC; }
static int term_create(const char* path)  { (void)path; return VFS_EGENERIC; }

static vfs_ops_t terminal_ops = {
    .open    = term_open,
    .read    = term_read,
    .write   = term_write,
    .close   = term_close,
    .unlink  = term_unlink,
    .readdir = term_readdir,
    .mkdir   = term_mkdir,
    .rmdir   = term_rmdir,
    .create  = term_create,
};

// ---- VFS init / mount ------------------------------------------------------

void vfs_init(void)
{
    mounted_fs = NULL;
    klog_info("VFS: initialized");
}

int vfs_mount(vfs_ops_t* ops)
{
    if (!ops)
        return VFS_EGENERIC;

    if (mounted_fs) {
        klog_warn("VFS: mount failed — filesystem already mounted");
        return VFS_EEXIST;
    }

    if (!ops->open   || !ops->read  || !ops->write ||
        !ops->close  || !ops->unlink|| !ops->readdir||
        !ops->mkdir  || !ops->rmdir || !ops->create) {
        klog_warn("VFS: mount failed — incomplete ops vtable");
        return VFS_EGENERIC;
    }

    mounted_fs = ops;
    klog_info("VFS: filesystem mounted at /");
    return VFS_EOK;
}

// ---- stdio setup -----------------------------------------------------------

// Populate fds 0/1/2 of `task` with terminal pseudo-files.
// Called from task_create() / task_create_user() after the task struct exists.
// We write directly into task->fds[] rather than going through vfs_alloc_fd()
// so we don't depend on scheduler_current() pointing at the new task yet.
void vfs_setup_stdio(task_t* task)
{
    // stdin (fd 0): readable terminal pseudo-file
    vfs_file_t* in = (vfs_file_t*)kmalloc(sizeof(vfs_file_t));
    if (in) {
        in->flags         = O_RDONLY;
        in->pos           = 0;
        in->size          = 0;
        in->first_cluster = 0;
        in->is_dir        = 0;
        in->fs_data       = NULL;
        in->ops           = &terminal_ops;
        task->fds[VFS_FD_STDIN] = in;
    }

    // stdout (fd 1): writable terminal pseudo-file
    vfs_file_t* out = (vfs_file_t*)kmalloc(sizeof(vfs_file_t));
    if (out) {
        out->flags         = O_WRONLY;
        out->pos           = 0;
        out->size          = 0;
        out->first_cluster = 0;
        out->is_dir        = 0;
        out->fs_data       = NULL;
        out->ops           = &terminal_ops;
        task->fds[VFS_FD_STDOUT] = out;
    }

    // stderr (fd 2): writable terminal pseudo-file (same backing as stdout)
    vfs_file_t* err = (vfs_file_t*)kmalloc(sizeof(vfs_file_t));
    if (err) {
        err->flags         = O_WRONLY;
        err->pos           = 0;
        err->size          = 0;
        err->first_cluster = 0;
        err->is_dir        = 0;
        err->fs_data       = NULL;
        err->ops           = &terminal_ops;
        task->fds[VFS_FD_STDERR] = err;
    }
}

// ---- fd table helpers ------------------------------------------------------

int vfs_alloc_fd(vfs_file_t** out)
{
    task_t* task = scheduler_current();
    if (!task)
        return VFS_EBADF;

    // Start from 3 — fds 0/1/2 are reserved for stdio
    for (int i = 3; i < VFS_MAX_FDS; i++) {
        if (!task->fds[i]) {
            vfs_file_t* f = (vfs_file_t*)kmalloc(sizeof(vfs_file_t));
            if (!f)
                return VFS_EBADF;
            task->fds[i] = f;
            *out = f;
            return i;
        }
    }

    return VFS_EBADF;  // fd table full
}

vfs_file_t* vfs_get_fd(int fd)
{
    if (fd < 0 || fd >= VFS_MAX_FDS)
        return NULL;

    task_t* task = scheduler_current();
    if (!task)
        return NULL;

    return task->fds[fd];
}

void vfs_free_fd(int fd)
{
    if (fd < 0 || fd >= VFS_MAX_FDS)
        return;

    task_t* task = scheduler_current();
    if (!task)
        return;

    if (task->fds[fd]) {
        kfree(task->fds[fd]);
        task->fds[fd] = NULL;
    }
}

// Close all fds for an explicit task — used by task_free() / reap_drain()
// where the dead task is not scheduler_current().
void vfs_task_close_all(task_t* task)
{
    if (!task)
        return;

    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (task->fds[i]) {
            // Call the filesystem's close to flush any pending writes.
            // terminal_ops.close is a no-op; fat32_close flushes the dirent.
            task->fds[i]->ops->close(task->fds[i]);
            kfree(task->fds[i]);
            task->fds[i] = NULL;
        }
    }
}

void vfs_close_all(void)
{
    vfs_task_close_all(scheduler_current());
}

// ---- fd-based API ----------------------------------------------------------

int vfs_open(const char* path, uint32_t flags)
{
    if (!mounted_fs) {
        klog_warn("VFS: open(%s) — no filesystem mounted", path);
        return VFS_EGENERIC;
    }

    vfs_file_t* file;
    int fd = vfs_alloc_fd(&file);
    if (fd < 0) {
        klog_warn("VFS: open(%s) — fd table full", path);
        return VFS_EBADF;
    }

    int ret = mounted_fs->open(path, flags, file);
    if (ret < 0) {
        vfs_free_fd(fd);
        return ret;
    }

    file->ops = mounted_fs;
    return fd;
}

int vfs_read(int fd, void* buf, uint32_t len)
{
    vfs_file_t* file = vfs_get_fd(fd);
    if (!file)
        return VFS_EBADF;

    if (!(file->flags & O_RDONLY) && !(file->flags & O_RDWR))
        return VFS_EBADF;

    return file->ops->read(file, buf, len);
}

int vfs_write(int fd, const void* buf, uint32_t len)
{
    vfs_file_t* file = vfs_get_fd(fd);
    if (!file)
        return VFS_EBADF;

    if (!(file->flags & O_WRONLY) && !(file->flags & O_RDWR))
        return VFS_EBADF;

    return file->ops->write(file, buf, len);
}

int vfs_close(int fd)
{
    vfs_file_t* file = vfs_get_fd(fd);
    if (!file)
        return VFS_EBADF;

    // Protect stdio fds from being closed explicitly — they are cleaned up
    // by vfs_task_close_all() at task teardown.
    if (fd == VFS_FD_STDIN || fd == VFS_FD_STDOUT || fd == VFS_FD_STDERR)
        return VFS_EBADF;

    int ret = file->ops->close(file);
    vfs_free_fd(fd);
    return ret;
}

int vfs_readdir(int fd, uint32_t index, vfs_dirent_t* out)
{
    vfs_file_t* file = vfs_get_fd(fd);
    if (!file)
        return VFS_EBADF;

    if (!file->is_dir)
        return VFS_ENOTDIR;

    return file->ops->readdir(file, index, out);
}

int vfs_seek(int fd, uint32_t offset, int whence)
{
    vfs_file_t* file = vfs_get_fd(fd);
    if (!file)
        return VFS_EBADF;

    if (file->is_dir)
        return VFS_EBADF;

    uint32_t new_pos;
    switch (whence) {
        case VFS_SEEK_SET:
            new_pos = offset;
            break;
        case VFS_SEEK_CUR:
            new_pos = file->pos + offset;
            break;
        case VFS_SEEK_END:
            new_pos = file->size + offset;
            break;
        default:
            return VFS_EGENERIC;
    }

    file->pos = new_pos;
    return (int)new_pos;
}

// ---- path-based API --------------------------------------------------------

int vfs_mkdir(const char* path)
{
    if (!mounted_fs) return VFS_EGENERIC;
    return mounted_fs->mkdir(path);
}

int vfs_rmdir(const char* path)
{
    if (!mounted_fs) return VFS_EGENERIC;
    return mounted_fs->rmdir(path);
}

int vfs_create(const char* path)
{
    if (!mounted_fs) return VFS_EGENERIC;
    return mounted_fs->create(path);
}

int vfs_unlink(const char* path)
{
    if (!mounted_fs) return VFS_EGENERIC;
    return mounted_fs->unlink(path);
}