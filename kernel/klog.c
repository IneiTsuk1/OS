#include "klog.h"
#include "kprintf.h"
#include "../drivers/vga/terminal.h"
#include "../drivers/serial/serial.h"
#include <stdarg.h>

static void klog_prefix(const char* level)
{
    kprintf("[%s] ", level);
}

static void klog_internal(const char* level, const char* fmt, va_list args)
{
    uint8_t log_color;

    if      (level[0] == 'I') log_color = VGA_COLOR_GREEN;
    else if (level[0] == 'W') log_color = VGA_COLOR_YELLOW;
    else                      log_color = VGA_COLOR_RED;

    terminal_set_color(log_color);
    klog_prefix(level);
    vkprintf(fmt, args);
    kprintf("\n");
    terminal_set_color(VGA_COLOR_WHITE);
}

void klog_info(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    klog_internal("INFO", fmt, args);
    va_end(args);
}

void klog_warn(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    klog_internal("WARN", fmt, args);
    va_end(args);
}

void klog_error(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    klog_internal("ERROR", fmt, args);
    va_end(args);
}