#pragma once
#include <stdint.h>

// Multiboot2 memory region types
#define MMAP_TYPE_AVAILABLE  1  // free RAM
#define MMAP_TYPE_RESERVED   2  // reserved by hardware/firmware
#define MMAP_TYPE_ACPI       3  // ACPI reclaimable
#define MMAP_TYPE_NVS        4  // ACPI NVS (must preserve across sleep)
#define MMAP_TYPE_BADRAM     5  // defective RAM

typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
} mmap_entry_t;

#define MMAP_MAX_ENTRIES 64

extern mmap_entry_t mmap_entries[];
extern int          mmap_entry_count;

// Parse the Multiboot2 info block passed by GRUB.
void mmap_init(uint32_t mb2_info);

// Log all detected memory regions.
void mmap_dump(void);