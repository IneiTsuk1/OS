#pragma once
/* stdio.h — printf, putchar, puts, and file I/O over syscalls. */
#include <stdint.h>
#include <stdarg.h>
#include "string.h"   /* size_t */

/* Standard I/O: write to stdout (fd 1). */
int putchar(int c);
int puts(const char* s);          /* writes s + '\n' */
int printf(const char* fmt, ...); /* supports: %c %s %d %u %x %% */
int fprintf(int fd, const char* fmt, ...);
int vfprintf(int fd, const char* fmt, va_list args);

/* Formatted string into buffer (no fd I/O). */
int sprintf(char* buf, const char* fmt, ...);
int vsprintf(char* buf, const char* fmt, va_list args);