#include "mmap.h"
#include "klog.h"
#include "panic.h"
#include <stdint.h>

// ---------------------------------------------------------------------------
// Multiboot2 tag structure definitions (only what we need)
// Spec: https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html
// ---------------------------------------------------------------------------

// Every MB2 tag starts with this header
typedef struct {
    uint32_t type;
    uint32_t size;
} __attribute__((packed)) mb2_tag_t;

// The memory map tag (type=6) is followed by entries of this layout
typedef struct {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
} __attribute__((packed)) mb2_mmap_entry_t;

typedef struct {
    uint32_t type;          // 6
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    mb2_mmap_entry_t entries[];
} __attribute__((packed)) mb2_mmap_tag_t;

// ---------------------------------------------------------------------------

mmap_entry_t mmap_entries[MMAP_MAX_ENTRIES];
int          mmap_entry_count = 0;

void mmap_init(uint32_t mb2_info)
{
    if (mb2_info == 0)
        panic("mmap_init: null Multiboot2 info pointer");

    // MB2 info block: first 8 bytes are total_size + reserved, then tags follow
    uint32_t total_size = *(uint32_t*)mb2_info;

    // Walk tags starting at mb2_info + 8
    uint32_t offset = 8;

    while (offset < total_size) {
        mb2_tag_t* tag = (mb2_tag_t*)(mb2_info + offset);

        if (tag->type == 0)     // type 0 = end tag
            break;

        if (tag->type == 6) {   // type 6 = memory map
            mb2_mmap_tag_t* mmap_tag = (mb2_mmap_tag_t*)(mb2_info + offset);

            uint32_t entries_size = mmap_tag->size - sizeof(mb2_mmap_tag_t);
            uint32_t num_entries  = entries_size / mmap_tag->entry_size;

            for (uint32_t i = 0; i < num_entries; i++) {
                if (mmap_entry_count >= MMAP_MAX_ENTRIES)
                    break;

                mb2_mmap_entry_t* e = (mb2_mmap_entry_t*)(
                    (uint32_t)mmap_tag->entries + i * mmap_tag->entry_size
                );

                mmap_entries[mmap_entry_count].base   = e->base_addr;
                mmap_entries[mmap_entry_count].length = e->length;
                mmap_entries[mmap_entry_count].type   = e->type;
                mmap_entry_count++;
            }
        }

        // Tags are 8-byte aligned
        offset += (tag->size + 7) & ~7u;
    }

    if (mmap_entry_count == 0)
        panic("mmap_init: no memory map entries found");
}

void mmap_dump(void)
{
    static const char* type_names[] = {
        "?", "Available", "Reserved", "ACPI", "NVS", "BadRAM"
    };

    klog_info("Memory map (%d regions):", mmap_entry_count);

    for (int i = 0; i < mmap_entry_count; i++) {
        mmap_entry_t* e = &mmap_entries[i];

        const char* name = (e->type <= 5) ? type_names[e->type] : "Unknown";

        // Print base and length as 32-bit values (we're in 32-bit mode;
        // anything above 4GiB won't be accessible anyway)
        klog_info("  [%d] base=%x len=%x type=%s",
            i,
            (uint32_t)e->base,
            (uint32_t)e->length,
            name);
    }
}