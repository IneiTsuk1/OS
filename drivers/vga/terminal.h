#pragma once
#include <stdint.h>

// VGA colour codes (foreground)
#define VGA_COLOR_WHITE        0x0F
#define VGA_COLOR_GREEN        0x0A
#define VGA_COLOR_YELLOW       0x0E
#define VGA_COLOR_RED          0x0C
#define VGA_COLOR_LIGHT_CYAN   0x0B

void terminal_set_color(uint8_t color);

void terminal_init(void);
void terminal_clear(void);
void terminal_putchar(char c);
void terminal_write(const char* str);