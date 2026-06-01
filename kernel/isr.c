#include "isr.h"
#include "klog.h"
#include "syscall.h"
#include "panic.h"

// Intel SDM Vol.3 Table 6-1: all 32 reserved exception vectors
static const char* exception_msgs[32] =
{
    "Divide-by-Zero",               //  0 #DE
    "Debug",                        //  1 #DB
    "Non-Maskable Interrupt",       //  2
    "Breakpoint",                   //  3 #BP
    "Overflow",                     //  4 #OF
    "Bound Range Exceeded",         //  5 #BR
    "Invalid Opcode",               //  6 #UD
    "Device Not Available",         //  7 #NM
    "Double Fault",                 //  8 #DF
    "Coprocessor Segment Overrun",  //  9 (legacy, never fires on modern CPUs)
    "Invalid TSS",                  // 10 #TS
    "Segment Not Present",          // 11 #NP
    "Stack-Segment Fault",          // 12 #SS
    "General Protection Fault",     // 13 #GP
    "Page Fault",                   // 14 #PF
    "Reserved",                     // 15
    "x87 FP Exception",             // 16 #MF
    "Alignment Check",              // 17 #AC
    "Machine Check",                // 18 #MC
    "SIMD FP Exception",            // 19 #XM/#XF
    "Virtualisation Exception",     // 20 #VE
    "Control Protection Exception", // 21 #CP
    "Reserved",                     // 22
    "Reserved",                     // 23
    "Reserved",                     // 24
    "Reserved",                     // 25
    "Reserved",                     // 26
    "Reserved",                     // 27
    "Hypervisor Injection",         // 28 #HV
    "VMM Communication Exception",  // 29 #VC
    "Security Exception",           // 30 #SX
    "Reserved",                     // 31
};

extern void idt_set_gate(int, uint32_t, uint16_t, uint8_t);

extern void isr0(void);
extern void isr1(void);
extern void isr2(void);
extern void isr3(void);
extern void isr4(void);
extern void isr5(void);
extern void isr6(void);
extern void isr7(void);
extern void isr8(void);
extern void isr9(void);
extern void isr10(void);
extern void isr11(void);
extern void isr12(void);
extern void isr13(void);
extern void isr14(void);
extern void isr15(void);
extern void isr16(void);
extern void isr17(void);
extern void isr18(void);
extern void isr19(void);
extern void isr20(void);
extern void isr21(void);
extern void isr22(void);
extern void isr23(void);
extern void isr24(void);
extern void isr25(void);
extern void isr26(void);
extern void isr27(void);
extern void isr28(void);
extern void isr29(void);
extern void isr30(void);
extern void isr31(void);
extern void isr128(void);


void isr_install(void)
{
    // Selector 0x08 = kernel code segment; flags 0x8E = present, ring 0, interrupt gate
    idt_set_gate(0,  (uint32_t)isr0,  0x08, 0x8E);
    idt_set_gate(1,  (uint32_t)isr1,  0x08, 0x8E);
    idt_set_gate(2,  (uint32_t)isr2,  0x08, 0x8E);
    idt_set_gate(3,  (uint32_t)isr3,  0x08, 0x8E);
    idt_set_gate(4,  (uint32_t)isr4,  0x08, 0x8E);
    idt_set_gate(5,  (uint32_t)isr5,  0x08, 0x8E);
    idt_set_gate(6,  (uint32_t)isr6,  0x08, 0x8E);
    idt_set_gate(7,  (uint32_t)isr7,  0x08, 0x8E);
    idt_set_gate(8,  (uint32_t)isr8,  0x08, 0x8E);
    idt_set_gate(9,  (uint32_t)isr9,  0x08, 0x8E);
    idt_set_gate(10, (uint32_t)isr10, 0x08, 0x8E);
    idt_set_gate(11, (uint32_t)isr11, 0x08, 0x8E);
    idt_set_gate(12, (uint32_t)isr12, 0x08, 0x8E);
    idt_set_gate(13, (uint32_t)isr13, 0x08, 0x8E);
    idt_set_gate(14, (uint32_t)isr14, 0x08, 0x8E);
    idt_set_gate(15, (uint32_t)isr15, 0x08, 0x8E);
    idt_set_gate(16, (uint32_t)isr16, 0x08, 0x8E);
    idt_set_gate(17, (uint32_t)isr17, 0x08, 0x8E);
    idt_set_gate(18, (uint32_t)isr18, 0x08, 0x8E);
    idt_set_gate(19, (uint32_t)isr19, 0x08, 0x8E);
    idt_set_gate(20, (uint32_t)isr20, 0x08, 0x8E);
    idt_set_gate(21, (uint32_t)isr21, 0x08, 0x8E);
    idt_set_gate(22, (uint32_t)isr22, 0x08, 0x8E);
    idt_set_gate(23, (uint32_t)isr23, 0x08, 0x8E);
    idt_set_gate(24, (uint32_t)isr24, 0x08, 0x8E);
    idt_set_gate(25, (uint32_t)isr25, 0x08, 0x8E);
    idt_set_gate(26, (uint32_t)isr26, 0x08, 0x8E);
    idt_set_gate(27, (uint32_t)isr27, 0x08, 0x8E);
    idt_set_gate(28, (uint32_t)isr28, 0x08, 0x8E);
    idt_set_gate(29, (uint32_t)isr29, 0x08, 0x8E);
    idt_set_gate(30, (uint32_t)isr30, 0x08, 0x8E);
    idt_set_gate(31, (uint32_t)isr31, 0x08, 0x8E);
    idt_set_gate(128, (uint32_t)isr128, 0x08, 0x8E);
}

void isr_handler(regs_t* r)
{
    if (r->int_no == 128) {         // 0x80 — syscall
        syscall_handler(r);
        return;
    }

    const char* msg = (r->int_no < 32)
                      ? exception_msgs[r->int_no]
                      : "Unknown Exception";

    klog_error("EXCEPTION #%u: %s", r->int_no, msg);
    klog_error("  EIP=%x  CS=%x  EFLAGS=%x", r->eip, r->cs, r->eflags);
    klog_error("  EAX=%x  EBX=%x  ECX=%x  EDX=%x", r->eax, r->ebx, r->ecx, r->edx);
    klog_error("  ESI=%x  EDI=%x  EBP=%x  ESP=%x", r->esi, r->edi, r->ebp, r->esp);
    klog_error("  ERR_CODE=%x", r->err_code);

    panic("CPU halted due to unhandled exception");
}