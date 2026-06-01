#include "pic.h"
#include <stdint.h>

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define PIC_EOI   0x20

#define ICW1_INIT 0x10
#define ICW1_ICW4 0x01
#define ICW4_8086 0x01

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void io_wait(void)
{
    outb(0x80, 0);  // write to unused port — ~1us delay for old hardware
}

void pic_init(void)
{
    // Save masks
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    // Start initialisation sequence (cascade mode)
    outb(PIC1_CMD,  ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC2_CMD,  ICW1_INIT | ICW1_ICW4); io_wait();

    // ICW2: vector offsets
    outb(PIC1_DATA, 0x20); io_wait();  // IRQ0-7  → INT 32-39
    outb(PIC2_DATA, 0x28); io_wait();  // IRQ8-15 → INT 40-47

    // ICW3: cascade wiring
    outb(PIC1_DATA, 0x04); io_wait();  // master: slave on IRQ2
    outb(PIC2_DATA, 0x02); io_wait();  // slave: cascade identity 2

    // ICW4: 8086 mode
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    // Restore masks
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

void pic_eoi(uint8_t irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

void pic_mask(uint8_t irq)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = irq % 8;
    outb(port, inb(port) | (1u << bit));
}

void pic_unmask(uint8_t irq)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = irq % 8;
    outb(port, inb(port) & ~(1u << bit));
}