#pragma once
/* syscall.h — user-space int 0x80 wrappers for MyOS.
 *
 * This header is for user programs only.  It has no dependency on kernel
 * headers (no isr.h, no kernel types beyond stdint.h).
 *
 * Calling convention: number in EAX, args in EBX / ECX / EDX.
 * Return value in EAX (negative = error).
 */
#include <stdint.h>

/* ---- syscall numbers ------------------------------------------------------ */
#define SYS_WRITE    1
#define SYS_SLEEP    2
#define SYS_OPEN     3
#define SYS_READ     4
#define SYS_FWRITE   5
#define SYS_CLOSE    6
#define SYS_SEEK     7
#define SYS_STAT     8
#define SYS_EXEC     9
#define SYS_WAITPID  10
#define SYS_EXIT     60

/* ---- open flags ----------------------------------------------------------- */
#define O_RDONLY  0x01
#define O_WRONLY  0x02
#define O_RDWR    0x03
#define O_CREAT   0x04
#define O_APPEND  0x08

/* ---- seek origins --------------------------------------------------------- */
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

/* ---- standard file descriptors -------------------------------------------- */
#define STDIN_FILENO   0
#define STDOUT_FILENO  1
#define STDERR_FILENO  2

/* ---- stat result ---------------------------------------------------------- */
typedef struct {
    uint32_t size;
    uint32_t is_dir;
} sys_stat_t;

/* ---- inline syscall wrappers ---------------------------------------------- */

static inline int sys_write(const char* msg)
{
    int ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_WRITE), "b"(msg)
        : "memory"
    );
    return ret;
}

static inline void sys_sleep(unsigned int ms)
{
    __asm__ volatile (
        "int $0x80"
        :: "a"(SYS_SLEEP), "b"(ms)
    );
}

static inline int sys_open(const char* path, unsigned int flags)
{
    int ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_OPEN), "b"(path), "c"(flags)
        : "memory"
    );
    return ret;
}

static inline int sys_read(int fd, void* buf, unsigned int len)
{
    int ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_READ), "b"(fd), "c"(buf), "d"(len)
        : "memory"
    );
    return ret;
}

static inline int sys_fwrite(int fd, const void* buf, unsigned int len)
{
    int ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_FWRITE), "b"(fd), "c"(buf), "d"(len)
        : "memory"
    );
    return ret;
}

static inline int sys_close(int fd)
{
    int ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_CLOSE), "b"(fd)
    );
    return ret;
}

static inline int sys_seek(int fd, int offset, int whence)
{
    int ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_SEEK), "b"(fd), "c"(offset), "d"(whence)
    );
    return ret;
}

static inline int sys_stat(const char* path, sys_stat_t* st)
{
    int ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_STAT), "b"(path), "c"(st)
        : "memory"
    );
    return ret;
}

static inline int sys_exec(const char* path, const char** argv)
{
    int ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_EXEC), "b"(path), "c"(argv)
        : "memory"
    );
    return ret;
}

static inline void sys_waitpid(int tid)
{
    __asm__ volatile (
        "int $0x80"
        :: "a"(SYS_WAITPID), "b"(tid)
        : "memory"
    );
}

static inline void sys_exit(int code)
{
    __asm__ volatile (
        "int $0x80"
        :: "a"(SYS_EXIT), "b"(code)
    );
    while (1) {}
}