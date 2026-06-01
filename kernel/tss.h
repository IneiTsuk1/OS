#pragma once
#include <stdint.h>

// Initialise the TSS, write its descriptor into GDT slot 3 (selector 0x18),
// and load TR.  `kernel_esp` should be the top of the boot kernel stack;
// call tss_set_kernel_stack() on every subsequent task switch.
void tss_init(uint32_t kernel_esp);

// Update tss.esp0 to point to the top of the current task's kernel stack.
// Must be called by the scheduler before every return to user mode or switch
// to a different task, so the CPU uses the right kernel stack on the next
// privilege-level transition.
void tss_set_kernel_stack(uint32_t esp0);