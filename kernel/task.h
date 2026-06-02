#pragma once
#include <stdint.h>

#define TASK_STACK_SIZE       8192        // 8 KiB kernel stack per task
#define USER_STACK_VIRT       0xBFFFF000u // virtual address of the user stack page
#define USER_STACK_TOP        0xC0000000u // user ESP on first entry (top of that page)

// VFS_MAX_FDS defined here so task.h is self-contained.
// vfs.h uses this value via an extern or its own define — they must agree.
#define VFS_MAX_FDS 16

typedef enum {
    TASK_READY   = 0,
    TASK_RUNNING = 1,
    TASK_BLOCKED = 2,
    TASK_DEAD    = 3,
} task_state_t;

// Forward declaration — breaks the task.h <-> vfs.h include cycle.
// vfs_file_t is defined in kernel/vfs/vfs.h; task.h only needs a pointer.
struct vfs_file;

// Forward declaration for elf_load_result_t (defined in elf.h).
// task.h only needs a pointer to avoid pulling in elf.h here.
typedef struct elf_load_result elf_load_result_t;

typedef struct task {
    uint32_t        esp;             // saved kernel stack pointer (top of saved frame)
    uint32_t        stack_top;       // highest address of kernel stack (for TSS)
    uint8_t*        stack_base;      // lowest address of kernel stack (what kmalloc returned)
    uint32_t        tid;             // task ID
    task_state_t    state;
    uint32_t        wake_tick;       // pit tick at which TASK_BLOCKED becomes TASK_READY
    uint32_t*       page_dir;        // virtual address of page directory
    uint32_t        page_dir_phys;   // physical address of page directory (for CR3 load)
    int             is_user;         // 0 = kernel task, 1 = user task
    uint32_t        user_stack_virt; // virtual address of user stack page (for cleanup)
    struct task*    waiter;          // task blocked in SYS_WAITPID on this task, or NULL
    struct task*    next;            // intrusive linked list — run queue

    // ---- signals ---------------------------------------------------------
    // Bitmask of pending signals.  Bit N = signal N (SIGKILL = bit 9).
    // Set by scheduler_kill(); checked in scheduler_tick().
    uint32_t        pending_signals;

    // Exit code recorded by scheduler_exit() and returned by SYS_WAITPID.
    // For normal exits this is the value passed to SYS_EXIT.
    // For signal-caused exits this is 128 + signal number (POSIX convention).
    int             exit_code;

    // ---- file descriptor table -------------------------------------------
    // fds[i] is NULL (slot closed) or a kmalloc'd vfs_file_t* (slot open).
    // vfs_alloc_fd() allocates the struct; vfs_free_fd() frees it.
    struct vfs_file* fds[VFS_MAX_FDS];
} task_t;

// Create a kernel-mode task. Entry point runs at CPL=0.
task_t* task_create(void (*entry)(void));

// Create a user-mode task. Entry point runs at CPL=3.
task_t* task_create_user(void (*entry)(void));

// Create a user-mode task from a loaded ELF image.
// `elf` must have been filled by elf_load(); this function takes ownership of
// elf->dir_phys (do NOT call paging_destroy_user_dir on it after this returns).
// Entry point and page directory are taken from `elf`.
// `argc` and `argv` are copied onto the user stack in the standard i386 ABI
// layout so that _start receives them correctly.  argv[0] should be the
// program path.  Pass argc=0 / argv=NULL to start with an empty argument list.
task_t* task_create_user_from_elf(struct elf_load_result* elf,
                                   int argc, const char** argv);

// Free all resources owned by a task (kernel stack, user page dir if any).
// Must NOT be called on tid 0 (idle task uses the boot stack).
void task_free(task_t* task);