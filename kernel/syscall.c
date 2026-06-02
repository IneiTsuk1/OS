#include "syscall.h"
#include "isr.h"
#include "idt.h"
#include "scheduler.h"
#include "task.h"
#include "paging.h"
#include "elf.h"
#include "pipe.h"
#include "vfs/vfs.h"
#include "../drivers/timer/pit.h"
#include "klog.h"
#include "kprintf.h"
#include "kheap.h"
#include <stdint.h>

// Maximum byte length (including null terminator) accepted from SYS_WRITE.
#define SYS_WRITE_MAX_LEN  4096

void syscall_handler(regs_t* r)
{
    switch (r->eax) {

        case SYS_WRITE: {
            task_t*  task = scheduler_current();
            uint32_t ptr  = r->ebx;

            if (!paging_user_str_ok(task->page_dir_phys, ptr, SYS_WRITE_MAX_LEN)) {
                klog_warn("SYS_WRITE tid=%u: bad pointer %x (EFAULT)", task->tid, ptr);
                r->eax = (uint32_t)-1;
                break;
            }

            const char* msg = (const char*)ptr;
            uint32_t len = 0;
            while (msg[len]) len++;

            int n = vfs_write(VFS_FD_STDOUT, msg, len);
            r->eax = (n < 0) ? (uint32_t)n : (uint32_t)len;
            break;
        }

        case SYS_SLEEP: {
            uint32_t ms = r->ebx;
            klog_info("[user tid=%u] sleep(%u ms)", scheduler_current()->tid, ms);
            __asm__ volatile ("sti");
            pit_sleep(ms);
            __asm__ volatile ("cli");
            r->eax = 0;
            break;
        }

        case SYS_OPEN: {
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
            int fd = (int)r->ebx;
            klog_info("[user tid=%u] close(fd=%d)", scheduler_current()->tid, fd);
            r->eax = (uint32_t)vfs_close(fd);
            break;
        }

        case SYS_SEEK: {
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
            task_t*  task     = scheduler_current();
            uint32_t path_ptr = r->ebx;
            uint32_t stat_ptr = r->ecx;

            if (!paging_user_str_ok(task->page_dir_phys, path_ptr, VFS_MAX_PATH)) {
                klog_warn("SYS_STAT tid=%u: bad path pointer %x", task->tid, path_ptr);
                r->eax = (uint32_t)VFS_EFAULT;
                break;
            }

            if (!paging_user_ptr_ok(task->page_dir_phys, stat_ptr) ||
                !paging_user_ptr_ok(task->page_dir_phys, stat_ptr + 4)) {
                klog_warn("SYS_STAT tid=%u: bad stat pointer %x", task->tid, stat_ptr);
                r->eax = (uint32_t)VFS_EFAULT;
                break;
            }

            const char* path = (const char*)path_ptr;
            int fd = vfs_open(path, O_RDONLY);
            if (fd < 0) { r->eax = (uint32_t)VFS_ENOENT; break; }

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
            int code = (int)r->ebx;
            klog_info("[user tid=%u] exit(%d)", scheduler_current()->tid, code);
            scheduler_exit(code);
            r->eax = 0;
            break;
        }

        case SYS_EXEC: {
            // EBX = path, ECX = argv[], EDX = stdout redirect path or 0.
            //
            // Redirect lifetime and dirent flush:
            //   We open the redirect file (rfd) in the shell's fd table.
            //   The child gets a shallow copy of the vfs_file_t with the
            //   REAL fs_data pointer (fat32_file_state_t) so fat32_close
            //   on the child's fd flushes the correct final size to disk.
            //   The shell's rfd has its fs_data zeroed before vfs_close so
            //   fat32_close on the shell side is a no-op and does not
            //   double-free or overwrite the dirent with size=0.
            task_t*  task      = scheduler_current();
            uint32_t path_ptr  = r->ebx;
            uint32_t argv_ptr  = r->ecx;
            uint32_t redir_ptr = r->edx;

            if (task->is_user) {
                if (!paging_user_str_ok(task->page_dir_phys, path_ptr, VFS_MAX_PATH)) {
                    klog_warn("SYS_EXEC tid=%u: bad path pointer %x", task->tid, path_ptr);
                    r->eax = (uint32_t)-1;
                    break;
                }
            }

            const char* path = (const char*)path_ptr;
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
            if (redir_ptr) {
                const char* redir_path = (const char*)redir_ptr;

                vfs_unlink(redir_path);
                vfs_create(redir_path);

                int rfd = vfs_open(redir_path, O_WRONLY);
                if (rfd >= 0) {
                    vfs_file_t* src = vfs_get_fd(rfd);
                    if (src) {
                        // Child stdout: full copy including fs_data so
                        // fat32_close on child exit flushes the final size.
                        vfs_file_t* dst = (vfs_file_t*)kmalloc(sizeof(vfs_file_t));
                        if (dst) {
                            for (uint32_t b = 0; b < sizeof(vfs_file_t); b++)
                                ((uint8_t*)dst)[b] = ((uint8_t*)src)[b];
                            dst->flags = O_WRONLY;
                            // fs_data is shared — child owns it for flushing
                            if (new_task->fds[VFS_FD_STDOUT])
                                kfree(new_task->fds[VFS_FD_STDOUT]);
                            new_task->fds[VFS_FD_STDOUT] = dst;
                        }

                        // Child stderr: also redirected, but NULL fs_data
                        // since only one copy should flush the dirent.
                        vfs_file_t* dst2 = (vfs_file_t*)kmalloc(sizeof(vfs_file_t));
                        if (dst2) {
                            for (uint32_t b = 0; b < sizeof(vfs_file_t); b++)
                                ((uint8_t*)dst2)[b] = ((uint8_t*)src)[b];
                            dst2->flags   = O_WRONLY;
                            dst2->fs_data = NULL; // stdout copy owns the flush
                            if (new_task->fds[VFS_FD_STDERR])
                                kfree(new_task->fds[VFS_FD_STDERR]);
                            new_task->fds[VFS_FD_STDERR] = dst2;
                        }

                        // Shell's rfd: zero fs_data so our close is a no-op
                        // (child owns the state and will flush on exit).
                        src->fs_data = NULL;
                        klog_info("SYS_EXEC: stdout redirected to %s", redir_path);
                    }
                    // Close shell's rfd — fat32_close sees fs_data=NULL, no flush.
                    vfs_close(rfd);
                } else {
                    klog_warn("SYS_EXEC: redirect open(%s) failed", redir_path);
                }
            }

            new_task->waiter = scheduler_current();
            scheduler_add(new_task);
            klog_info("SYS_EXEC: launched tid=%u entry=%x argc=%d",
                new_task->tid, elf.entry, argc);
            r->eax = (uint32_t)new_task->tid;
            break;
        }

        case SYS_EXECP: {
            // EBX = path, ECX = argv[], EDX = stdout_fd (-1=none), ESI = stdin_fd (-1=none)
            task_t*  task      = scheduler_current();
            uint32_t path_ptr  = r->ebx;
            uint32_t argv_ptr  = r->ecx;
            int      stdout_fd = (int)r->edx;
            int      stdin_fd  = (int)r->esi;

            if (task->is_user) {
                if (!paging_user_str_ok(task->page_dir_phys, path_ptr, VFS_MAX_PATH)) {
                    klog_warn("SYS_EXECP tid=%u: bad path pointer %x", task->tid, path_ptr);
                    r->eax = (uint32_t)-1;
                    break;
                }
            }

            const char* path = (const char*)path_ptr;
            const char** argv = (const char**)argv_ptr;
            int argc = 0;
            if (argv) { while (argc < 64 && argv[argc]) argc++; }

            klog_info("[user tid=%u] execp(%s) argc=%d stdout_fd=%d stdin_fd=%d",
                task->tid, path, argc, stdout_fd, stdin_fd);

            int fd = vfs_open(path, O_RDONLY);
            if (fd < 0) {
                klog_warn("SYS_EXECP: open(%s) failed (%d)", path, fd);
                r->eax = (uint32_t)fd;
                break;
            }

            elf_load_result_t elf;
            int ret = elf_load(fd, &elf);
            vfs_close(fd);

            if (ret < 0) {
                klog_warn("SYS_EXECP: elf_load failed for %s", path);
                r->eax = (uint32_t)-1;
                break;
            }

            task_t* new_task = task_create_user_from_elf(&elf, argc, argv);
            if (!new_task) {
                paging_destroy_user_dir(elf.dir_phys);
                klog_warn("SYS_EXECP: task_create_user_from_elf OOM");
                r->eax = (uint32_t)-1;
                break;
            }

            // Wire stdout: shallow-copy with refcount increment
            if (stdout_fd >= 0 && stdout_fd < VFS_MAX_FDS && task->fds[stdout_fd]) {
                vfs_file_t* src = task->fds[stdout_fd];
                vfs_file_t* dst = (vfs_file_t*)kmalloc(sizeof(vfs_file_t));
                if (dst) {
                    for (uint32_t b = 0; b < sizeof(vfs_file_t); b++)
                        ((uint8_t*)dst)[b] = ((uint8_t*)src)[b];
                    kfree(new_task->fds[VFS_FD_STDOUT]);
                    new_task->fds[VFS_FD_STDOUT] = dst;
                    pipe_ref_write((pipe_t*)dst->fs_data);
                    klog_info("SYS_EXECP: child stdout -> caller fd %d (pipe)", stdout_fd);
                }
            }

            // Wire stdin: shallow-copy with refcount increment
            if (stdin_fd >= 0 && stdin_fd < VFS_MAX_FDS && task->fds[stdin_fd]) {
                vfs_file_t* src = task->fds[stdin_fd];
                vfs_file_t* dst = (vfs_file_t*)kmalloc(sizeof(vfs_file_t));
                if (dst) {
                    for (uint32_t b = 0; b < sizeof(vfs_file_t); b++)
                        ((uint8_t*)dst)[b] = ((uint8_t*)src)[b];
                    kfree(new_task->fds[VFS_FD_STDIN]);
                    new_task->fds[VFS_FD_STDIN] = dst;
                    pipe_ref_read((pipe_t*)dst->fs_data);
                    klog_info("SYS_EXECP: child stdin -> caller fd %d (pipe)", stdin_fd);
                }
            }

            new_task->waiter = scheduler_current();
            scheduler_add(new_task);
            klog_info("SYS_EXECP: launched tid=%u entry=%x", new_task->tid, elf.entry);
            r->eax = (uint32_t)new_task->tid;
            break;
        }

        case SYS_WAITPID: {
            uint32_t tid = r->ebx;
            klog_info("[user tid=%u] waitpid(%u)", scheduler_current()->tid, tid);
            __asm__ volatile ("sti");
            int code = scheduler_wait(tid);
            __asm__ volatile ("cli");
            r->eax = (uint32_t)code;
            break;
        }

        case SYS_KILL: {
            uint32_t tid = r->ebx;
            int      sig = (int)r->ecx;
            klog_info("[user tid=%u] kill(tid=%u, sig=%d)",
                scheduler_current()->tid, tid, sig);
            int ret = scheduler_kill(tid, sig);
            r->eax = (uint32_t)ret;
            break;
        }

        case SYS_PIPE: {
            task_t*  task    = scheduler_current();
            uint32_t arr_ptr = r->ecx;

            if (task->is_user) {
                if (!paging_user_ptr_ok(task->page_dir_phys, arr_ptr) ||
                    !paging_user_ptr_ok(task->page_dir_phys, arr_ptr + 4)) {
                    klog_warn("SYS_PIPE tid=%u: bad fds pointer %x", task->tid, arr_ptr);
                    r->eax = (uint32_t)VFS_EFAULT;
                    break;
                }
            }

            vfs_file_t* rf = NULL;
            vfs_file_t* wf = NULL;
            if (pipe_create(&rf, &wf) < 0) {
                klog_warn("SYS_PIPE tid=%u: pipe_create OOM", task->tid);
                r->eax = (uint32_t)-1;
                break;
            }

            int rfd = vfs_alloc_fd_for(task, rf);
            if (rfd < 0) {
                rf->ops->close(rf); kfree(rf);
                wf->ops->close(wf); kfree(wf);
                r->eax = (uint32_t)VFS_EBADF;
                break;
            }

            int wfd = vfs_alloc_fd_for(task, wf);
            if (wfd < 0) {
                task->fds[rfd] = NULL;
                rf->ops->close(rf); kfree(rf);
                wf->ops->close(wf); kfree(wf);
                r->eax = (uint32_t)VFS_EBADF;
                break;
            }

            int* fds = (int*)arr_ptr;
            fds[0] = rfd;
            fds[1] = wfd;

            klog_info("[user tid=%u] pipe() = [%d, %d]", task->tid, rfd, wfd);
            r->eax = 0;
            break;
        }

        case SYS_DUP2: {
            task_t* task  = scheduler_current();
            int     oldfd = (int)r->ebx;
            int     newfd = (int)r->ecx;

            if (oldfd < 0 || oldfd >= VFS_MAX_FDS ||
                newfd < 0 || newfd >= VFS_MAX_FDS) {
                r->eax = (uint32_t)VFS_EBADF; break;
            }

            vfs_file_t* src = task->fds[oldfd];
            if (!src) { r->eax = (uint32_t)VFS_EBADF; break; }

            if (oldfd == newfd) { r->eax = (uint32_t)newfd; break; }

            if (task->fds[newfd]) {
                task->fds[newfd]->ops->close(task->fds[newfd]);
                kfree(task->fds[newfd]);
                task->fds[newfd] = NULL;
            }

            vfs_file_t* dup = (vfs_file_t*)kmalloc(sizeof(vfs_file_t));
            if (!dup) { r->eax = (uint32_t)-1; break; }

            uint8_t* s = (uint8_t*)src;
            uint8_t* d = (uint8_t*)dup;
            for (uint32_t i = 0; i < sizeof(vfs_file_t); i++) d[i] = s[i];

            task->fds[newfd] = dup;
            klog_info("[user tid=%u] dup2(%d, %d)", task->tid, oldfd, newfd);
            r->eax = (uint32_t)newfd;
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