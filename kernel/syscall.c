#include "syscall.h"
#include "isr.h"
#include "idt.h"
#include "scheduler.h"
#include "task.h"
#include "paging.h"
#include "elf.h"
#include "vfs/vfs.h"
#include "../drivers/timer/pit.h"
#include "klog.h"
#include "kprintf.h"
#include <stdint.h>

// Maximum byte length (including null terminator) accepted from SYS_WRITE.
#define SYS_WRITE_MAX_LEN  4096

void syscall_handler(regs_t* r)
{
    switch (r->eax) {

        case SYS_WRITE: {
            // EBX = pointer to a null-terminated string in user space.
            // Writes the string to the task's stdout (fd 1) so output appears
            // directly on the terminal as a clean stream, not wrapped in log lines.
            task_t*  task = scheduler_current();
            uint32_t ptr  = r->ebx;

            if (!paging_user_str_ok(task->page_dir_phys, ptr, SYS_WRITE_MAX_LEN)) {
                klog_warn("SYS_WRITE tid=%u: bad pointer %x (EFAULT)", task->tid, ptr);
                r->eax = (uint32_t)-1;
                break;
            }

            // Measure string length so we can pass it to vfs_write.
            const char* msg = (const char*)ptr;
            uint32_t len = 0;
            while (msg[len]) len++;

            // Write to stdout (fd 1).  vfs_write on the terminal pseudo-driver
            // calls terminal_putchar + serial_write_char for each byte.
            int n = vfs_write(VFS_FD_STDOUT, msg, len);
            r->eax = (n < 0) ? (uint32_t)n : (uint32_t)len;
            break;
        }

        case SYS_SLEEP: {
            // EBX = sleep duration in milliseconds.
            // Re-enable interrupts before sleeping: scheduler_sleep() blocks on
            // hlt which requires IF=1.  Re-disable after so isr_common invariants
            // hold on return.
            uint32_t ms = r->ebx;
            klog_info("[user tid=%u] sleep(%u ms)", scheduler_current()->tid, ms);

            __asm__ volatile ("sti");
            pit_sleep(ms);
            __asm__ volatile ("cli");

            r->eax = 0;
            break;
        }

        case SYS_OPEN: {
            // EBX = ptr to path string (user space), ECX = flags
            task_t*  task  = scheduler_current();
            uint32_t ptr   = r->ebx;
            uint32_t flags = r->ecx;

            if (!paging_user_str_ok(task->page_dir_phys, ptr, VFS_MAX_PATH)) {
                klog_warn("SYS_OPEN tid=%u: bad path pointer %x", task->tid, ptr);
                r->eax = (uint32_t)VFS_EFAULT;
                break;
            }

            const char* path = (const char*)ptr;
            int fd = vfs_open(path, flags);
            klog_info("[user tid=%u] open(%s) = %d", task->tid, path, fd);
            r->eax = (uint32_t)fd;
            break;
        }

        case SYS_READ: {
            // EBX = fd, ECX = buf ptr (user space), EDX = len
            task_t*  task = scheduler_current();
            int      fd   = (int)r->ebx;
            uint32_t ptr  = r->ecx;
            uint32_t len  = r->edx;

            for (uint32_t off = 0; off < len; off += 4096) {
                if (!paging_user_ptr_ok(task->page_dir_phys, ptr + off)) {
                    r->eax = (uint32_t)VFS_EFAULT;
                    goto done;
                }
            }

            {
                // Re-enable interrupts: if fd==0 (stdin), vfs_read will block
                // via hlt inside keyboard_read, which requires IF=1.
                // For non-stdin fds the read returns immediately so sti/cli
                // is a no-op cost-wise.
                __asm__ volatile ("sti");
                int n = vfs_read(fd, (void*)ptr, len);
                __asm__ volatile ("cli");
                klog_info("[user tid=%u] read(fd=%d, len=%u) = %d",
                    task->tid, fd, len, n);
                r->eax = (uint32_t)n;
            }
            done:
            break;
        }

        case SYS_FWRITE: {
            // EBX = fd, ECX = buf ptr (user space), EDX = len
            task_t*  task = scheduler_current();
            int      fd   = (int)r->ebx;
            uint32_t ptr  = r->ecx;
            uint32_t len  = r->edx;

            for (uint32_t off = 0; off < len; off += 4096) {
                if (!paging_user_ptr_ok(task->page_dir_phys, ptr + off)) {
                    r->eax = (uint32_t)VFS_EFAULT;
                    goto fwrite_done;
                }
            }

            {
                int n = vfs_write(fd, (const void*)ptr, len);
                klog_info("[user tid=%u] fwrite(fd=%d, len=%u) = %d",
                    task->tid, fd, len, n);
                r->eax = (uint32_t)n;
            }
            fwrite_done:
            break;
        }

        case SYS_CLOSE: {
            // EBX = fd
            int fd = (int)r->ebx;
            klog_info("[user tid=%u] close(fd=%d)", scheduler_current()->tid, fd);
            r->eax = (uint32_t)vfs_close(fd);
            break;
        }

        case SYS_SEEK: {
            // EBX = fd, ECX = offset, EDX = whence
            int      fd     = (int)r->ebx;
            uint32_t offset = r->ecx;
            int      whence = (int)r->edx;
            int      result = vfs_seek(fd, offset, whence);
            klog_info("[user tid=%u] seek(fd=%d, off=%u, whence=%d) = %d",
                scheduler_current()->tid, fd, offset, whence, result);
            r->eax = (uint32_t)result;
            break;
        }

        case SYS_STAT: {
            // EBX = ptr to path string (user space)
            // ECX = ptr to sys_stat_t (user space) — we write size + is_dir
            task_t*  task     = scheduler_current();
            uint32_t path_ptr = r->ebx;
            uint32_t stat_ptr = r->ecx;

            if (!paging_user_str_ok(task->page_dir_phys, path_ptr, VFS_MAX_PATH)) {
                klog_warn("SYS_STAT tid=%u: bad path pointer %x", task->tid, path_ptr);
                r->eax = (uint32_t)VFS_EFAULT;
                break;
            }

            // Validate both dwords of sys_stat_t in user space.
            if (!paging_user_ptr_ok(task->page_dir_phys, stat_ptr) ||
                !paging_user_ptr_ok(task->page_dir_phys, stat_ptr + 4)) {
                klog_warn("SYS_STAT tid=%u: bad stat pointer %x", task->tid, stat_ptr);
                r->eax = (uint32_t)VFS_EFAULT;
                break;
            }

            const char* path = (const char*)path_ptr;
            int fd = vfs_open(path, O_RDONLY);
            if (fd < 0) {
                r->eax = (uint32_t)VFS_ENOENT;
                break;
            }

            vfs_file_t* file = vfs_get_fd(fd);
            sys_stat_t* st   = (sys_stat_t*)stat_ptr;
            st->size   = file->size;
            st->is_dir = file->is_dir;
            vfs_close(fd);

            klog_info("[user tid=%u] stat(%s) size=%u is_dir=%u",
                task->tid, path, st->size, st->is_dir);
            r->eax = 0;
            break;
        }

        case SYS_EXIT: {
            uint32_t code = r->ebx;
            klog_info("[user tid=%u] exit(%u)", scheduler_current()->tid, code);
            scheduler_exit();
            r->eax = 0;
            break;
        }

        case SYS_EXEC: {
            // EBX = ptr to path string (user or kernel space).
            // ECX = ptr to null-terminated char* argv[] array (argv[0] = path).
            //       May be NULL — treated as argc=0.
            // EDX = ptr to stdout redirect path string, or 0 for no redirect.
            //       If non-zero, the child's fd 1 (stdout) is replaced with a
            //       newly created/truncated file at that path before the task runs.
            // Loads and launches an ELF32 executable.
            // Returns the new task's tid in EAX, or negative on error.
            task_t*  task      = scheduler_current();
            uint32_t path_ptr  = r->ebx;
            uint32_t argv_ptr  = r->ecx;
            uint32_t redir_ptr = r->edx;   // stdout redirect path, or 0

            // Kernel tasks (e.g. the shell) pass kernel pointers — PAGE_USER
            // is not set on kernel pages so skip user-pointer validation for them.
            if (task->is_user) {
                if (!paging_user_str_ok(task->page_dir_phys, path_ptr, VFS_MAX_PATH)) {
                    klog_warn("SYS_EXEC tid=%u: bad path pointer %x", task->tid, path_ptr);
                    r->eax = (uint32_t)-1;
                    break;
                }
            }

            const char* path = (const char*)path_ptr;

            // Build argc/argv from the caller-supplied array.
            // Kernel callers pass a kernel char** directly; we trust it.
            // Cap at 64 args for safety.
            const char** argv = (const char**)argv_ptr;
            int argc = 0;
            if (argv) {
                while (argc < 64 && argv[argc])
                    argc++;
            }

            klog_info("[user tid=%u] exec(%s) argc=%d", task->tid, path, argc);

            int fd = vfs_open(path, O_RDONLY);
            if (fd < 0) {
                klog_warn("SYS_EXEC: open(%s) failed (%d)", path, fd);
                r->eax = (uint32_t)fd;
                break;
            }

            elf_load_result_t elf;
            int ret = elf_load(fd, &elf);
            vfs_close(fd);

            if (ret < 0) {
                klog_warn("SYS_EXEC: elf_load failed for %s", path);
                r->eax = (uint32_t)-1;
                break;
            }

            task_t* new_task = task_create_user_from_elf(&elf, argc, argv);
            if (!new_task) {
                paging_destroy_user_dir(elf.dir_phys);
                klog_warn("SYS_EXEC: task_create_user_from_elf OOM");
                r->eax = (uint32_t)-1;
                break;
            }

            // ---- stdout redirect --------------------------------------------
            // If a redirect path was supplied, open/create the file and replace
            // the child's stdout (fd 1) with it.  We do this before
            // scheduler_add so the task never runs with the wrong stdout.
            if (redir_ptr) {
                const char* redir_path = (const char*)redir_ptr;

                // Truncate any existing file by unlinking then creating fresh.
                vfs_unlink(redir_path);
                vfs_create(redir_path);

                // Allocate a vfs_file_t directly into the new task's fd 1.
                // We can't use vfs_open (it operates on scheduler_current's table),
                // so open in our own table, copy the struct, then close our fd.
                int rfd = vfs_open(redir_path, O_WRONLY);
                if (rfd >= 0) {
                    vfs_file_t* src = vfs_get_fd(rfd);
                    vfs_file_t* dst = (vfs_file_t*)kmalloc(sizeof(vfs_file_t));
                    if (dst && src) {
                        // Copy the open file state into a fresh allocation.
                        for (uint32_t b = 0; b < sizeof(vfs_file_t); b++)
                            ((uint8_t*)dst)[b] = ((uint8_t*)src)[b];
                        dst->flags = O_WRONLY;

                        // Replace child's stdout — free the terminal pseudo-fd first.
                        if (new_task->fds[VFS_FD_STDOUT])
                            kfree(new_task->fds[VFS_FD_STDOUT]);
                        new_task->fds[VFS_FD_STDOUT] = dst;

                        // Also replace stderr so error output goes to the file.
                        vfs_file_t* dst2 = (vfs_file_t*)kmalloc(sizeof(vfs_file_t));
                        if (dst2) {
                            for (uint32_t b = 0; b < sizeof(vfs_file_t); b++)
                                ((uint8_t*)dst2)[b] = ((uint8_t*)src)[b];
                            dst2->flags = O_WRONLY;
                            if (new_task->fds[VFS_FD_STDERR])
                                kfree(new_task->fds[VFS_FD_STDERR]);
                            new_task->fds[VFS_FD_STDERR] = dst2;
                        }

                        klog_info("SYS_EXEC: stdout redirected to %s", redir_path);
                    } else if (dst) {
                        kfree(dst);
                    }
                    // Close our temporary fd — the child has its own copy.
                    vfs_close(rfd);
                } else {
                    klog_warn("SYS_EXEC: redirect open(%s) failed", redir_path);
                }
            }

            // Pre-register the calling task as the waiter BEFORE scheduler_add.
            // This closes the race where the child runs to completion between
            // scheduler_add() returning and SYS_WAITPID being called: if the
            // child exits before SYS_WAITPID, scheduler_exit() finds waiter set
            // and wakes us; scheduler_wait() then finds target==NULL and returns
            // immediately since we are already TASK_READY.
            new_task->waiter = scheduler_current();

            scheduler_add(new_task);
            klog_info("SYS_EXEC: launched tid=%u entry=%x argc=%d",
                new_task->tid, elf.entry, argc);
            r->eax = (uint32_t)new_task->tid;
            break;
        }

        case SYS_WAITPID: {
            // EBX = tid of the task to wait for.
            // Blocks until the target task exits (or returns immediately if it
            // has already exited and been reaped).
            // Re-enable interrupts: scheduler_wait blocks on hlt (requires IF=1).
            uint32_t tid = r->ebx;
            klog_info("[user tid=%u] waitpid(%u)", scheduler_current()->tid, tid);

            __asm__ volatile ("sti");
            scheduler_wait(tid);
            __asm__ volatile ("cli");

            r->eax = 0;
            break;
        }

        default:
            klog_warn("syscall: unknown number %u from tid=%u",
                r->eax, scheduler_current()->tid);
            r->eax = (uint32_t)-1;
            break;
    }
}

void syscall_init(void)
{
    idt_set_gate(0x80, (uint32_t)isr128, 0x08, 0xEE);
    klog_info("Syscall: int 0x80 gate installed");
}