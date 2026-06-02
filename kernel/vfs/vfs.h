#pragma once
#include <stdint.h>
#include <stddef.h>

// Forward declaration — breaks the vfs.h <-> task.h include cycle.
// task_t is defined in kernel/task.h; vfs.h only needs a pointer.
typedef struct task task_t;

// ---------------------------------------------------------------------------
// VFS — Virtual File System
//
// Minimal top-down design for M7.  Only one mount point (root "/") is
// supported.  A single registered filesystem handles all path lookups.
//
// File descriptor integers are stored per-task in task_t.fds[].
// Kernel code may also use vfs_file_t* directly without going through fds.
// ---------------------------------------------------------------------------

#define VFS_MAX_FDS        16       // open files per task
#define VFS_MAX_PATH       256      // maximum path length including null
#define VFS_MAX_NAME       256      // maximum filename length including null

// ---- directory entry -------------------------------------------------------

typedef struct {
    char     name[VFS_MAX_NAME];    // null-terminated filename
    uint8_t  is_dir;                // 1 = directory, 0 = regular file
    uint32_t size;                  // file size in bytes (0 for directories)
    uint32_t cluster;               // first cluster (filesystem-private)
} vfs_dirent_t;

// ---- open file handle ------------------------------------------------------

typedef struct vfs_file {
    uint32_t  flags;                // O_RDONLY / O_WRONLY / O_RDWR
    uint32_t  pos;                  // current byte offset
    uint32_t  size;                 // file size in bytes
    uint32_t  first_cluster;        // filesystem-private: starting cluster
    uint8_t   is_dir;               // 1 if this is a directory handle
    void*     fs_data;              // filesystem-private per-file state
    struct vfs_ops* ops;            // pointer back to the fs vtable
} vfs_file_t;

// ---- filesystem vtable -----------------------------------------------------
//
// A filesystem registers itself by filling in this struct and passing it to
// vfs_mount().  Every function pointer must be non-NULL.
//
// Return conventions (matching Unix errno style):
//   0        = success
//  -1        = generic error
//  -ENOENT   = path not found   (use literal -2)
//  -EEXIST   = already exists   (use literal -3)
//  -ENOSPC   = no space left    (use literal -4)
//  -ENOTDIR  = not a directory  (use literal -5)
//  -EISDIR   = is a directory   (use literal -6)
//  -EBADF    = bad file handle  (use literal -7)
//  -EFAULT   = bad pointer      (use literal -8)

#define VFS_EOK       0
#define VFS_EGENERIC (-1)
#define VFS_ENOENT   (-2)
#define VFS_EEXIST   (-3)
#define VFS_ENOSPC   (-4)
#define VFS_ENOTDIR  (-5)
#define VFS_EISDIR   (-6)
#define VFS_EBADF    (-7)
#define VFS_EFAULT   (-8)

// Open flags
#define O_RDONLY  0x01
#define O_WRONLY  0x02
#define O_RDWR    0x03
#define O_CREAT   0x04    // create if not exists
#define O_APPEND  0x08    // writes always go to end of file

typedef struct vfs_ops {
    // Open a file at `path`.  On success, fills `*out` and returns 0.
    // Creates the file if O_CREAT is set and it does not exist.
    int (*open)(const char* path, uint32_t flags, vfs_file_t* out);

    // Read up to `len` bytes from `file` at its current position into `buf`.
    // Advances file->pos.  Returns bytes read, or negative error code.
    int (*read)(vfs_file_t* file, void* buf, uint32_t len);

    // Write `len` bytes from `buf` into `file` at its current position
    // (or at end if O_APPEND).  Advances file->pos.
    // Returns bytes written, or negative error code.
    int (*write)(vfs_file_t* file, const void* buf, uint32_t len);

    // Close and release any filesystem-private state for `file`.
    // The vfs_file_t itself is owned by the caller (stack or fd table).
    int (*close)(vfs_file_t* file);

    // Delete the file at `path`.  Fails with VFS_EISDIR if it is a directory.
    int (*unlink)(const char* path);

    // Read one directory entry from an open directory handle.
    // `index` is the zero-based entry index (caller increments each call).
    // Returns 0 and fills `*out` on success; VFS_ENOENT when index is past
    // the last entry; negative error otherwise.
    int (*readdir)(vfs_file_t* dir, uint32_t index, vfs_dirent_t* out);

    // Create a directory at `path`.
    int (*mkdir)(const char* path);

    // Remove an empty directory at `path`.
    int (*rmdir)(const char* path);

    // Create a regular file at `path`.  Fails with VFS_EEXIST if it exists.
    int (*create)(const char* path);

    // Rename / move `src` to `dst`.
    // - If `dst` names an existing file, it is replaced atomically.
    // - If `dst` names an existing empty directory and `src` is a directory,
    //   `dst` is removed and `src` takes its place.
    // - Fails with VFS_EISDIR  if `dst` is a directory but `src` is not.
    // - Fails with VFS_ENOTDIR if `src` is a directory but `dst` is a file.
    // - Fails with VFS_EGENERIC if `dst` is a non-empty directory.
    // Both paths must be on the same volume (cross-device rename unsupported).
    int (*rename)(const char* src, const char* dst);

} vfs_ops_t;

// ---- public VFS API --------------------------------------------------------

// Standard file descriptor numbers — reserved in every task's fd table.
#define VFS_FD_STDIN   0
#define VFS_FD_STDOUT  1
#define VFS_FD_STDERR  2

// Initialise the VFS subsystem.  Must be called before any other vfs_ call.
void vfs_init(void);

// Register a filesystem.  Only one mount (root "/") is supported for M7.
// `ops` must remain valid for the lifetime of the kernel.
// Returns 0 on success, negative on error (e.g. already mounted).
int vfs_mount(vfs_ops_t* ops);

// ---- fd-based API (used by syscalls and kernel code) ----------------------

// Open a file and assign a file descriptor in the current task's fd table.
// Returns fd >= 0 on success, negative VFS error code on failure.
int vfs_open(const char* path, uint32_t flags);

// Read up to `len` bytes from `fd` into `buf`.
// Returns bytes read, or negative error code.
int vfs_read(int fd, void* buf, uint32_t len);

// Write `len` bytes from `buf` into `fd`.
// Returns bytes written, or negative error code.
int vfs_write(int fd, const void* buf, uint32_t len);

// Close file descriptor `fd`.
int vfs_close(int fd);

// Read directory entry `index` from open directory `fd`.
int vfs_readdir(int fd, uint32_t index, vfs_dirent_t* out);

// ---- path-based API (convenience wrappers, no fd allocation) ---------------

int vfs_mkdir(const char* path);
int vfs_rmdir(const char* path);
int vfs_create(const char* path);
int vfs_unlink(const char* path);
int vfs_rename(const char* src, const char* dst);

// ---- fd table helpers (used by task.c / scheduler.c) ----------------------

// Allocate a vfs_file_t from the current task's fd table.
// Returns the fd index, or negative if the table is full.
int vfs_alloc_fd(vfs_file_t** out);

// Insert an already-allocated vfs_file_t* into an explicit task's fd table.
// Finds the first free slot (>= 3) and stores `file` there.
// Returns the fd index on success, or VFS_EBADF if the table is full.
// Does NOT allocate or initialise the file — caller owns that.
// Used by SYS_PIPE to place pre-built pipe file structs into a task's table.
int vfs_alloc_fd_for(task_t* task, vfs_file_t* file);

// Populate fds 0/1/2 of `task` with terminal pseudo-files (stdin/stdout/stderr).
// Called from task_create() and task_create_user() after the task struct is ready.
void vfs_setup_stdio(task_t* task);

// Look up fd in the current task's table.
// Returns pointer to the file, or NULL if fd is invalid / not open.
vfs_file_t* vfs_get_fd(int fd);

// Release fd slot (called by vfs_close and task teardown).
void vfs_free_fd(int fd);

// Close all open fds for the current task.  Called from task_free().
void vfs_close_all(void);

// Close all open fds for an explicit task.  Use this from task_free() /
// reap_drain() where the dead task is NOT scheduler_current().
void vfs_task_close_all(task_t* task);

// Seek fd to `offset` bytes from `whence` (SEEK_SET=0, SEEK_CUR=1, SEEK_END=2).
// Returns the new position, or negative error code.
#define VFS_SEEK_SET 0
#define VFS_SEEK_CUR 1
#define VFS_SEEK_END 2
int vfs_seek(int fd, uint32_t offset, int whence);