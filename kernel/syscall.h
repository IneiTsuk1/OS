#pragma once
#include <stdint.h>
#include "isr.h"

// Syscall numbers — passed in EAX by the user task
#define SYS_WRITE    1    // EBX = ptr to null-terminated string; returns bytes written or -1 (EFAULT)
#define SYS_SLEEP    2    // EBX = milliseconds; returns 0
#define SYS_OPEN     3    // EBX = ptr to path string, ECX = flags; returns fd or negative
#define SYS_READ     4    // EBX = fd, ECX = ptr to buf, EDX = len; returns bytes read
#define SYS_FWRITE   5    // EBX = fd, ECX = ptr to buf, EDX = len; returns bytes written
#define SYS_CLOSE    6    // EBX = fd; returns 0 or negative
#define SYS_SEEK     7    // EBX = fd, ECX = offset, EDX = whence (0=SET,1=CUR,2=END);
                          //   returns new position or negative error
#define SYS_STAT     8    // EBX = ptr to path string, ECX = ptr to sys_stat_t in user space;
                          //   returns 0 or negative error
#define SYS_EXEC     9    // EBX = ptr to path, ECX = argv[]; returns new tid or negative error
#define SYS_WAITPID  10   // EBX = tid; blocks until that task exits; returns 0
#define SYS_EXIT     60   // EBX = exit code; does not return

// Stat result written into user space by SYS_STAT.
typedef struct {
    uint32_t size;       // file size in bytes (0 for directories)
    uint32_t is_dir;     // 1 = directory, 0 = regular file
} sys_stat_t;

// The actual dispatcher — called from isr_handler when int_no == 128.
void syscall_handler(regs_t* r);

// Install the int 0x80 IDT gate. Called once from kernel_main after idt_init.
void syscall_init(void);