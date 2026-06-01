#pragma once
/* stdio.h — printf, putchar, puts, and file I/O over syscalls. */
#include <stdint.h>
#include <stdarg.h>
#include "string.h"   /* size_t */

/* Standard I/O: write to stdout (fd 1). */
int putchar(int c);
int puts(const char* s);                             /* writes s + '\n' */
int printf(const char* fmt, ...);
int fprintf(int fd, const char* fmt, ...);
int vfprintf(int fd, const char* fmt, va_list args);

/* Bounded formatting — prefer these over sprintf/vsprintf in new code. */
int snprintf(char* buf, size_t size, const char* fmt, ...);
int vsnprintf(char* buf, size_t size, const char* fmt, va_list args);

/* Unbounded formatting into caller-supplied buffer (legacy, no bounds check). */
int sprintf(char* buf, const char* fmt, ...);
int vsprintf(char* buf, const char* fmt, va_list args);