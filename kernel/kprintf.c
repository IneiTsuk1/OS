#include "kprintf.h"
#include "../drivers/vga/terminal.h"
#include "../drivers/serial/serial.h"
#include <stdarg.h>

static void kputc(char c)
{
    char buf[2] = {c, 0};
    terminal_write(buf);
    serial_write(buf);
}

static void kprint_str(const char* s)
{
    if (!s) return;
    while (*s)
        kputc(*s++);
}

static void kprint_int(int value)
{
    char buf[12];
    int i = 0;
    int neg = 0;

    if (value == 0)
    {
        kputc('0');
        return;
    }

    if (value < 0)
    {
        neg = 1;
        value = -value;
    }

    while (value)
    {
        buf[i++] = '0' + (value % 10);
        value /= 10;
    }

    if (neg)
        kputc('-');

    while (i--)
        kputc(buf[i]);
}

static void kprint_hex(unsigned int value)
{
    char* hex = "0123456789ABCDEF";

    kputc('0');
    kputc('x');

    for (int i = 28; i >= 0; i -= 4)
        kputc(hex[(value >> i) & 0xF]);
}

static void kprint_uint(unsigned int value)
{
    char buf[11];
    int i = 0;

    if (value == 0)
    {
        kputc('0');
        return;
    }

    while (value)
    {
        buf[i++] = '0' + (value % 10);
        value /= 10;
    }

    while (i--)
        kputc(buf[i]);
}

void vkprintf(const char* fmt, va_list args)
{
    for (int i = 0; fmt[i]; i++)
    {
        if (fmt[i] != '%')
        {
            kputc(fmt[i]);
            continue;
        }

        i++;

        switch (fmt[i])
        {
            case 's':
                kprint_str(va_arg(args, const char*));
                break;

            case 'd':
                kprint_int(va_arg(args, int));
                break;

            case 'x':
                kprint_hex(va_arg(args, unsigned int));
                break;

            case 'c':
                kputc((char)va_arg(args, int));
                break;

            case 'u':
                kprint_uint(va_arg(args, unsigned int));
                break;

            case '%':
                kputc('%');
                break;

            default:
                kputc('%');
                kputc(fmt[i]);
                break;
        }
    }
}


void kprintf(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vkprintf(fmt, args);
    va_end(args);
}