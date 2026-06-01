#include "panic.h"
#include "kprintf.h"
#include "../drivers/vga/terminal.h"
#include "../drivers/serial/serial.h"
#include <stdarg.h>

void panic(const char* fmt, ...)
{
    __asm__ volatile("cli"); // MUST be first

    terminal_write("\n*** KERNEL PANIC ***\n");
    serial_write("\n*** KERNEL PANIC ***\n");

    va_list args;
    va_start(args, fmt);
    vkprintf(fmt, args);
    va_end(args);

    terminal_write("\n");
    serial_write("\n");

    while (1)
        __asm__ volatile("hlt");
}