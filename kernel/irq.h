#pragma once
#include "isr.h"

typedef void (*irq_handler_t)(regs_t* r);

// Called once during boot — registers IDT gates 32-47
void irq_install(void);

// Register a handler for IRQ line 0-15
void irq_install_handler(int irq, irq_handler_t handler);

// Remove a handler
void irq_remove_handler(int irq);