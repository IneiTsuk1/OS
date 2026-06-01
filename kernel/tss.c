#include "tss.h"
#include <stdint.h>

typedef struct {
    uint32_t prev_tss;
    uint32_t esp0;       // kernel stack pointer — updated on every task switch
    uint32_t ss0;        // kernel stack segment
    uint32_t esp1;
    uint32_t ss1;
    uint32_t esp2;
    uint32_t ss2;
    uint32_t cr3;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax, ecx, edx, ebx;
    uint32_t esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed)) tss_t;

static tss_t tss;

// GDT lives in kernel_entry.asm — we reach in and write entry 3.
// GDT layout: [null 0x00][code 0x08][data 0x10][tss 0x18]
extern uint64_t gdt_start[];

static void gdt_set_tss(uint32_t base, uint32_t limit)
{
    uint64_t* entry = &gdt_start[3];

    *entry  = 0;
    *entry |= (uint64_t)(limit & 0xFFFF);             // limit[15:0]
    *entry |= (uint64_t)(base  & 0xFFFFFF)   << 16;   // base[23:0]
    *entry |= (uint64_t)0x89               << 40;     // present, ring0, TSS32 available
    *entry |= (uint64_t)((limit >> 16) & 0xF) << 48;  // limit[19:16]
    *entry |= (uint64_t)((base  >> 24) & 0xFF) << 56; // base[31:24]
}

static inline void ltr(uint16_t sel)
{
    __asm__ volatile ("ltr %0" :: "r"(sel));
}

void tss_init(uint32_t kernel_esp)
{
    uint8_t* p = (uint8_t*)&tss;
    for (uint32_t i = 0; i < sizeof(tss_t); i++)
        p[i] = 0;

    tss.ss0        = 0x10;          // kernel data segment
    tss.esp0       = kernel_esp;    // initial kernel stack (boot stack top)
    tss.iomap_base = sizeof(tss_t); // no I/O permission bitmap

    gdt_set_tss((uint32_t)&tss, sizeof(tss_t) - 1);

    ltr(0x18);
}

// Called by the scheduler on every context switch, before returning to
// user mode (or to a different kernel task).  The CPU loads tss.esp0 as
// the kernel stack pointer whenever a privilege-level transition occurs,
// so this must point to the TOP of the incoming task's kernel stack.
void tss_set_kernel_stack(uint32_t esp0)
{
    tss.esp0 = esp0;
}