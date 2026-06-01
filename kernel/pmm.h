#pragma once
#include <stdint.h>

#define PMM_PAGE_SIZE 4096

// Initialise the allocator from the parsed memory map.
// Must be called after mmap_init(). Not thread-safe — call once before
// interrupts and SMP are active.
void pmm_init(void);

// Allocate one 4 KiB physical frame.
// Thread-safe (internal spinlock). Returns the physical address, or 0 if OOM.
uint32_t pmm_alloc_frame(void);

// Free a previously allocated frame.
// Thread-safe. Panics on double-free or null/out-of-range address.
void pmm_free_frame(uint32_t addr);

// Log allocator stats (total / used / free frames).
void pmm_dump(void);