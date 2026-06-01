#pragma once
#include <stdint.h>

// Remap PIC so IRQ0-15 map to INT vectors 32-47
void pic_init(void);

// Send End-Of-Interrupt to the appropriate PIC(s)
void pic_eoi(uint8_t irq);

// Mask / unmask individual IRQ lines
void pic_mask(uint8_t irq);
void pic_unmask(uint8_t irq);