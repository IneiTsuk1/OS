#pragma once
#include <stdint.h>

// Top-level kernel entry point — called from kernel_entry.asm
// mb2_info: physical address of the Multiboot2 info block
void kernel_main(uint32_t mb2_info);