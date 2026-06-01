#include "stdio.h"
#include "syscall.h"
#include "string.h"

/* ---- low-level write ------------------------------------------------------ */

/* Write len bytes from buf to fd via SYS_FWRITE. */
static int fd_write(int fd, const char* buf, int len)
{
    if (len <= 0) return 0;
    return sys_fwrite(fd, buf, (unsigned int)len);
}

/* ---- putchar / puts ------------------------------------------------------- */

int putchar(int c)
{
    char ch = (char)c;
    fd_write(STDOUT_FILENO, &ch, 1);
    return c;
}

int puts(const char* s)
{
    int n = (int)strlen(s);
    fd_write(STDOUT_FILENO, s, n);
    fd_write(STDOUT_FILENO, "\n", 1);
    return n + 1;
}

/* ---- vsprintf ------------------------------------------------------------- */
/*
 * Formats into a caller-supplied buffer.  Never writes more than the buffer
 * can hold — the caller must provide a large enough buffer (no bounds checking
 * here; this is a freestanding CRT with no dynamic allocation in the formatter).
 *
 * Supported conversions:
 *   %c   — single character
 *   %s   — null-terminated string (NULL → "(null)")
 *   %d   — signed 32-bit decimal
 *   %u   — unsigned 32-bit decimal
 *   %x   — unsigned 32-bit lowercase hex (no "0x" prefix)
 *   %X   — unsigned 32-bit uppercase hex (no "0X" prefix)
 *   %p   — pointer as 0x%08X
 *   %%   — literal '%'
 *
 * Width and precision are not supported — use %s with pre-formatted strings
 * via itoa if alignment is needed.
 */
int vsprintf(char* buf, const char* fmt, va_list args)
{
    char* out = buf;

    while (*fmt) {
        if (*fmt != '%') {
            *out++ = *fmt++;
            continue;
        }
        fmt++;  /* skip '%' */

        switch (*fmt++) {
            case 'c': {
                *out++ = (char)va_arg(args, int);
                break;
            }
            case 's': {
                const char* s = va_arg(args, const char*);
                if (!s) s = "(null)";
                while (*s) *out++ = *s++;
                break;
            }
            case 'd': {
                int n = va_arg(args, int);
                if (n < 0) { *out++ = '-'; n = -n; }
                char tmp[12]; int i = 0;
                if (n == 0) { tmp[i++] = '0'; }
                else {
                    unsigned int un = (unsigned int)n;
                    while (un) { tmp[i++] = '0' + un % 10; un /= 10; }
                }
                while (i--) *out++ = tmp[i];
                break;
            }
            case 'u': {
                unsigned int n = va_arg(args, unsigned int);
                char tmp[12]; int i = 0;
                if (n == 0) { tmp[i++] = '0'; }
                else { while (n) { tmp[i++] = '0' + n % 10; n /= 10; } }
                while (i--) *out++ = tmp[i];
                break;
            }
            case 'x': {
                unsigned int n = va_arg(args, unsigned int);
                const char* hex = "0123456789abcdef";
                char tmp[9]; int i = 0;
                if (n == 0) { tmp[i++] = '0'; }
                else { while (n) { tmp[i++] = hex[n & 0xF]; n >>= 4; } }
                while (i--) *out++ = tmp[i];
                break;
            }
            case 'X': {
                unsigned int n = va_arg(args, unsigned int);
                const char* hex = "0123456789ABCDEF";
                char tmp[9]; int i = 0;
                if (n == 0) { tmp[i++] = '0'; }
                else { while (n) { tmp[i++] = hex[n & 0xF]; n >>= 4; } }
                while (i--) *out++ = tmp[i];
                break;
            }
            case 'p': {
                /* Pointer: 0x followed by 8 hex digits */
                unsigned int n = (unsigned int)(uintptr_t)va_arg(args, void*);
                const char* hex = "0123456789abcdef";
                *out++ = '0'; *out++ = 'x';
                for (int shift = 28; shift >= 0; shift -= 4)
                    *out++ = hex[(n >> shift) & 0xF];
                break;
            }
            case '%': {
                *out++ = '%';
                break;
            }
            default: {
                /* Unknown specifier — emit literally */
                *out++ = '%';
                *out++ = *(fmt - 1);
                break;
            }
        }
    }

    *out = '\0';
    return (int)(out - buf);
}

int sprintf(char* buf, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int n = vsprintf(buf, fmt, args);
    va_end(args);
    return n;
}

/* ---- vfprintf ------------------------------------------------------------- */
/*
 * Formats into a stack buffer and writes to fd in one syscall.
 * 1 KiB buffer is enough for a single printf line in practice.
 * For very long output, call printf multiple times.
 */
int vfprintf(int fd, const char* fmt, va_list args)
{
    char buf[1024];
    int n = vsprintf(buf, fmt, args);
    if (n > 0)
        fd_write(fd, buf, n);
    return n;
}

int fprintf(int fd, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int n = vfprintf(fd, fmt, args);
    va_end(args);
    return n;
}

int printf(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int n = vfprintf(STDOUT_FILENO, fmt, args);
    va_end(args);
    return n;
}