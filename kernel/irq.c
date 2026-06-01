#include "irq.h"
#include "idt.h"
#include "pic.h"
#include "klog.h"
#include "scheduler.h"
#include "task.h"
#include <stddef.h>

// Forward declarations for stubs in isr_stub.asm
extern void irq0(void);
extern void irq1(void);
extern void irq2(void);
extern void irq3(void);
extern void irq4(void);
extern void irq5(void);
extern void irq6(void);
extern void irq7(void);
extern void irq8(void);
extern void irq9(void);
extern void irq10(void);
extern void irq11(void);
extern void irq12(void);
extern void irq13(void);
extern void irq14(void);
extern void irq15(void);

static irq_handler_t handlers[16] = {0};

void irq_install(void)
{
    pic_init();

    idt_set_gate(32, (uint32_t)irq0,  0x08, 0x8E);
    idt_set_gate(33, (uint32_t)irq1,  0x08, 0x8E);
    idt_set_gate(34, (uint32_t)irq2,  0x08, 0x8E);
    idt_set_gate(35, (uint32_t)irq3,  0x08, 0x8E);
    idt_set_gate(36, (uint32_t)irq4,  0x08, 0x8E);
    idt_set_gate(37, (uint32_t)irq5,  0x08, 0x8E);
    idt_set_gate(38, (uint32_t)irq6,  0x08, 0x8E);
    idt_set_gate(39, (uint32_t)irq7,  0x08, 0x8E);
    idt_set_gate(40, (uint32_t)irq8,  0x08, 0x8E);
    idt_set_gate(41, (uint32_t)irq9,  0x08, 0x8E);
    idt_set_gate(42, (uint32_t)irq10, 0x08, 0x8E);
    idt_set_gate(43, (uint32_t)irq11, 0x08, 0x8E);
    idt_set_gate(44, (uint32_t)irq12, 0x08, 0x8E);
    idt_set_gate(45, (uint32_t)irq13, 0x08, 0x8E);
    idt_set_gate(46, (uint32_t)irq14, 0x08, 0x8E);
    idt_set_gate(47, (uint32_t)irq15, 0x08, 0x8E);
}

void irq_install_handler(int irq, irq_handler_t handler)
{
    if (irq < 0 || irq > 15) return;
    handlers[irq] = handler;
    pic_unmask(irq);
}

void irq_remove_handler(int irq)
{
    if (irq < 0 || irq > 15) return;
    handlers[irq] = NULL;
    pic_mask(irq);
}

// Called from irq_common in isr_stub.asm
void irq_handler(regs_t* r)
{
    uint8_t irq = r->int_no - 32;

    if (handlers[irq])
        handlers[irq](r);
    else
        klog_warn("IRQ%u: no handler registered", irq);

    pic_eoi(irq);

    // If the interrupted task is blocked (e.g. scheduler_wait's hlt loop),
    // trigger an immediate reschedule so the CPU is handed to a runnable task
    // without waiting for the next PIT timeslice.  Without this, a keyboard IRQ
    // wakes the blocked task's hlt, irq_stub sees need_reschedule==0, and irets
    // back to the blocked task — leaving a ready consumer starved until the next
    // PIT tick fires 20ms later.
    task_t* cur = scheduler_current();
    if (cur && cur->state == TASK_BLOCKED)
        scheduler_set_need_reschedule();
}