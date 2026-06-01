#include "terminal.h"

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

static uint16_t* const VGA = (uint16_t*)0xB8000;

static int     row   = 0;
static int     col   = 0;
static uint8_t color = 0x0F;  // white on black

static uint16_t make_entry(char c)
{
    return (uint16_t)c | ((uint16_t)color << 8);
}

static uint16_t make_blank(void)
{
    return make_entry(' ');
}

void terminal_init(void)
{
    terminal_clear();
}

void terminal_clear(void)
{
    for (int y = 0; y < VGA_HEIGHT; y++)
        for (int x = 0; x < VGA_WIDTH; x++)
            VGA[y * VGA_WIDTH + x] = make_blank();
    row = 0;
    col = 0;
}

static void scroll(void)
{
    for (int y = 1; y < VGA_HEIGHT; y++)
        for (int x = 0; x < VGA_WIDTH; x++)
            VGA[(y - 1) * VGA_WIDTH + x] = VGA[y * VGA_WIDTH + x];

    for (int x = 0; x < VGA_WIDTH; x++)
        VGA[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = make_blank();
}

static void newline(void)
{
    col = 0;
    row++;

    if (row >= VGA_HEIGHT) {
        scroll();
        row = VGA_HEIGHT - 1;
    }
}

void terminal_putchar(char c)
{
    if (c == '\n') {
        newline();
        return;
    }

    if (c == '\b') {
        // Move cursor back one, erase the character
        if (col > 0) {
            col--;
        } else if (row > 0) {
            // Wrap back to end of previous line
            row--;
            col = VGA_WIDTH - 1;
        }
        VGA[row * VGA_WIDTH + col] = make_blank();
        return;
    }

    VGA[row * VGA_WIDTH + col] = make_entry(c);
    col++;

    if (col >= VGA_WIDTH)
        newline();
}

void terminal_write(const char* str)
{
    for (int i = 0; str[i]; i++)
        terminal_putchar(str[i]);
}

void terminal_set_color(uint8_t c)
{
    color = c;
}