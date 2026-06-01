#include "stdio.h"
#include "syscall.h"
#include "string.h"

/* ---- low-level write ------------------------------------------------------ */

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

/* ---- vsnprintf ------------------------------------------------------------ */
/*
 * Core formatter.  Writes at most (size-1) characters into buf, always
 * null-terminates (if size > 0), and returns the number of characters that
 * would have been written had the buffer been large enough (excluding the
 * null terminator) — matching the C99 snprintf contract.
 *
 * Supported conversions:
 *   %c   — single character
 *   %s   — null-terminated string (NULL → "(null)")
 *   %d   — signed 32-bit decimal
 *   %u   — unsigned 32-bit decimal
 *   %x   — unsigned 32-bit lowercase hex
 *   %X   — unsigned 32-bit uppercase hex
 *   %p   — pointer as 0x%08x
 *   %%   — literal '%'
 *
 * Width field (decimal digits only, no '*') is supported for all conversions.
 * Padding is space-padded and left-aligned (no '-' flag yet).
 */
int vsnprintf(char* buf, size_t size, const char* fmt, va_list args)
{
    /* We write to a shadow pointer so we can count bytes past the buffer end. */
    char*  out   = buf;
    size_t left  = (size > 0) ? size - 1 : 0;  /* bytes left excl. terminator */
    int    total = 0;                            /* total chars produced        */

#define EMIT(c) do {                    \
    if (left > 0) { *out++ = (c); left--; } \
    total++;                            \
} while (0)

    while (*fmt) {
        if (*fmt != '%') {
            EMIT(*fmt++);
            continue;
        }
        fmt++;  /* skip '%' */

        /* Optional width field */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');

        switch (*fmt++) {
            case 'c': {
                int pad = width > 1 ? width - 1 : 0;
                while (pad--) EMIT(' ');
                EMIT((char)va_arg(args, int));
                break;
            }
            case 's': {
                const char* s = va_arg(args, const char*);
                if (!s) s = "(null)";
                int slen = 0;
                const char* t = s;
                while (*t++) slen++;
                int pad = width > slen ? width - slen : 0;
                while (pad--) EMIT(' ');
                while (*s) EMIT(*s++);
                break;
            }
            case 'd': {
                int n   = va_arg(args, int);
                int neg = (n < 0);
                if (neg) n = -n;
                char tmp[12]; int i = 0;
                if (n == 0) { tmp[i++] = '0'; }
                else {
                    unsigned int un = (unsigned int)n;
                    while (un) { tmp[i++] = '0' + un % 10; un /= 10; }
                }
                int total_chars = neg + i;
                int pad = width > total_chars ? width - total_chars : 0;
                while (pad--) EMIT(' ');
                if (neg) EMIT('-');
                while (i--) EMIT(tmp[i]);
                break;
            }
            case 'u': {
                unsigned int n = va_arg(args, unsigned int);
                char tmp[12]; int i = 0;
                if (n == 0) { tmp[i++] = '0'; }
                else { while (n) { tmp[i++] = '0' + n % 10; n /= 10; } }
                int pad = width > i ? width - i : 0;
                while (pad--) EMIT(' ');
                while (i--) EMIT(tmp[i]);
                break;
            }
            case 'x': {
                unsigned int n = va_arg(args, unsigned int);
                const char* hex = "0123456789abcdef";
                char tmp[9]; int i = 0;
                if (n == 0) { tmp[i++] = '0'; }
                else { while (n) { tmp[i++] = hex[n & 0xF]; n >>= 4; } }
                int pad = width > i ? width - i : 0;
                while (pad--) EMIT(' ');
                while (i--) EMIT(tmp[i]);
                break;
            }
            case 'X': {
                unsigned int n = va_arg(args, unsigned int);
                const char* hex = "0123456789ABCDEF";
                char tmp[9]; int i = 0;
                if (n == 0) { tmp[i++] = '0'; }
                else { while (n) { tmp[i++] = hex[n & 0xF]; n >>= 4; } }
                int pad = width > i ? width - i : 0;
                while (pad--) EMIT(' ');
                while (i--) EMIT(tmp[i]);
                break;
            }
            case 'p': {
                unsigned int n = (unsigned int)(uintptr_t)va_arg(args, void*);
                const char* hex = "0123456789abcdef";
                EMIT('0'); EMIT('x');
                for (int shift = 28; shift >= 0; shift -= 4)
                    EMIT(hex[(n >> shift) & 0xF]);
                break;
            }
            case '%': {
                EMIT('%');
                break;
            }
            default: {
                EMIT('%');
                EMIT(*(fmt - 1));
                break;
            }
        }
    }

#undef EMIT

    if (size > 0) *out = '\0';
    return total;
}

/* ---- vsprintf (legacy, no bounds checking) -------------------------------- */
/*
 * Kept for source compatibility.  Prefer vsnprintf in new code.
 * Passes INT_MAX as the size limit — the caller is responsible for providing
 * a buffer large enough for the formatted output.
 */
int vsprintf(char* buf, const char* fmt, va_list args)
{
    return vsnprintf(buf, (size_t)-1, fmt, args);
}

/* ---- snprintf / sprintf --------------------------------------------------- */

int snprintf(char* buf, size_t size, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, size, fmt, args);
    va_end(args);
    return n;
}

int sprintf(char* buf, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, (size_t)-1, fmt, args);
    va_end(args);
    return n;
}

/* ---- vfprintf ------------------------------------------------------------- */
/*
 * Formats into a stack buffer and writes to fd.
 *
 * The buffer is 2 KiB.  vsnprintf will never overflow it — it truncates at
 * (size - 1) bytes and returns the number of bytes the full output would have
 * needed, so callers can detect truncation if they care.
 */
#define FPRINTF_BUFSZ 2048

int vfprintf(int fd, const char* fmt, va_list args)
{
    char buf[FPRINTF_BUFSZ];
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    /* n is the number of chars the format would produce if buf were infinite.
     * We write min(n, FPRINTF_BUFSZ-1) bytes — the null terminator is not sent. */
    int to_write = n < FPRINTF_BUFSZ ? n : FPRINTF_BUFSZ - 1;
    if (to_write > 0)
        fd_write(fd, buf, to_write);
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