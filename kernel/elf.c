#include "elf.h"
#include "vfs/vfs.h"
#include "paging.h"
#include "pmm.h"
#include "klog.h"
#include <stdint.h>
#include <stddef.h>

// Maximum number of PT_LOAD segments we will process.
// Real ELFs typically have 2-4 (text, data, rodata, tls).
#define ELF_MAX_PHDRS 16

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Zero `len` bytes starting at kernel-accessible address `dst`.
static void elf_memset(uint8_t* dst, uint8_t val, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
        dst[i] = val;
}

// Copy `len` bytes from `src` to `dst`.
static void elf_memcpy(uint8_t* dst, const uint8_t* src, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
        dst[i] = src[i];
}

// Resolve a virtual address through an arbitrary page directory (identified by
// its physical address) to the kernel-accessible pointer for that byte.
// Under identity mapping, physical == virtual, so we walk the PTEs.
// Returns NULL if the page is not present.
static uint8_t* elf_resolve_virt(uint32_t dir_phys, uint32_t virt)
{
    uint32_t* dir   = (uint32_t*)paging_phys_to_virt(dir_phys);
    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;

    if (!(dir[pd_idx] & PAGE_PRESENT))
        return NULL;

    uint32_t* pt = (uint32_t*)paging_phys_to_virt(dir[pd_idx] & ~0xFFFu);
    if (!(pt[pt_idx] & PAGE_PRESENT))
        return NULL;

    uint32_t frame = pt[pt_idx] & ~0xFFFu;
    return (uint8_t*)paging_phys_to_virt(frame) + (virt & 0xFFFu);
}

// Map `page_count` freshly-allocated physical frames into `dir_phys` starting
// at virtual address `virt_base` (page-aligned) with `flags`.
// Returns 0 on success, -1 on OOM.
//
// NOTE: Frames are NOT guaranteed physically contiguous. Always use
// elf_resolve_virt() to obtain the kernel address for a given virtual page
// rather than doing arithmetic from the first frame's address.
static int map_pages(uint32_t dir_phys, uint32_t virt_base,
                     uint32_t page_count, uint32_t flags)
{
    for (uint32_t i = 0; i < page_count; i++) {
        uint32_t frame = pmm_alloc_frame();
        if (!frame) {
            klog_warn("elf_load: OOM mapping segment pages");
            return -1;
        }

        if (paging_map_into(dir_phys, virt_base + i * PMM_PAGE_SIZE,
                            frame, flags) != 0) {
            pmm_free_frame(frame);
            klog_warn("elf_load: paging_map_into failed");
            return -1;
        }
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int elf_load(int fd, elf_load_result_t* result)
{
    // ---- 1. Read and validate the ELF header --------------------------------

    elf32_ehdr_t ehdr;

    if (vfs_seek(fd, 0, VFS_SEEK_SET) < 0) {
        klog_warn("elf_load: seek to 0 failed");
        return -1;
    }

    if (vfs_read(fd, &ehdr, sizeof(ehdr)) != (int)sizeof(ehdr)) {
        klog_warn("elf_load: failed to read ELF header");
        return -1;
    }

    // Magic
    uint32_t magic = *(uint32_t*)ehdr.e_ident;
    if (magic != ELF_MAGIC) {
        klog_warn("elf_load: bad magic %x", magic);
        return -1;
    }

    // 32-bit, little-endian, executable, x86
    if (ehdr.e_ident[4] != ELF_CLASS_32) {
        klog_warn("elf_load: not a 32-bit ELF");
        return -1;
    }
    if (ehdr.e_ident[5] != ELF_DATA_LSB) {
        klog_warn("elf_load: not little-endian");
        return -1;
    }
    if (ehdr.e_type != ELF_TYPE_EXEC) {
        klog_warn("elf_load: not an executable (type=%u)", ehdr.e_type);
        return -1;
    }
    if (ehdr.e_machine != ELF_MACHINE_386) {
        klog_warn("elf_load: wrong machine (machine=%u)", ehdr.e_machine);
        return -1;
    }
    if (ehdr.e_phoff == 0 || ehdr.e_phnum == 0) {
        klog_warn("elf_load: no program headers");
        return -1;
    }
    if (ehdr.e_phentsize != sizeof(elf32_phdr_t)) {
        klog_warn("elf_load: unexpected phentsize %u", ehdr.e_phentsize);
        return -1;
    }
    if (ehdr.e_phnum > ELF_MAX_PHDRS) {
        klog_warn("elf_load: too many phdrs (%u)", ehdr.e_phnum);
        return -1;
    }

    // ---- 2. Read program headers --------------------------------------------

    elf32_phdr_t phdrs[ELF_MAX_PHDRS];

    if (vfs_seek(fd, ehdr.e_phoff, VFS_SEEK_SET) < 0) {
        klog_warn("elf_load: seek to phoff=%x failed", ehdr.e_phoff);
        return -1;
    }

    uint32_t phdrs_bytes = ehdr.e_phnum * sizeof(elf32_phdr_t);
    if (vfs_read(fd, phdrs, phdrs_bytes) != (int)phdrs_bytes) {
        klog_warn("elf_load: failed to read program headers");
        return -1;
    }

    // ---- 3. Allocate a fresh user address space ----------------------------

    uint32_t dir_phys = paging_create_user_dir();
    if (!dir_phys) {
        klog_warn("elf_load: OOM creating user page directory");
        return -1;
    }

    // ---- 4. Load each PT_LOAD segment --------------------------------------
    //
    // For each PT_LOAD segment:
    //   a. Round vaddr down to page boundary, round (vaddr+memsz) up.
    //   b. Allocate and map that many pages into dir_phys.
    //   c. Zero every mapped page individually via elf_resolve_virt()
    //      (frames are NOT contiguous — pointer arithmetic from first_frame
    //       is WRONG and was the source of kernel memory corruption).
    //   d. Copy p_filesz bytes from the file, one page-chunk at a time,
    //      again resolving each destination page through the PTEs.

    // Temporary buffer for reading file data one page at a time.
    uint8_t page_buf[PMM_PAGE_SIZE];

    for (uint32_t i = 0; i < ehdr.e_phnum; i++) {
        elf32_phdr_t* ph = &phdrs[i];

        if (ph->p_type != PT_LOAD)
            continue;
        if (ph->p_memsz == 0)
            continue;

        // Page-align the region to map.
        uint32_t virt_base  = ph->p_vaddr & ~(PMM_PAGE_SIZE - 1);
        uint32_t virt_end   = (ph->p_vaddr + ph->p_memsz + PMM_PAGE_SIZE - 1)
                              & ~(PMM_PAGE_SIZE - 1);
        uint32_t page_count = (virt_end - virt_base) / PMM_PAGE_SIZE;

        // Build PTE flags from ELF segment flags.
        uint32_t pte_flags = PAGE_USER;
        if (ph->p_flags & PF_W)
            pte_flags |= PAGE_WRITABLE;
        // x86 32-bit has no NX bit in basic paging; PF_X is implicit.

        if (map_pages(dir_phys, virt_base, page_count, pte_flags) != 0) {
            paging_destroy_user_dir(dir_phys);
            return -1;
        }

        // Zero every page in the segment individually.
        // We must resolve each page through the page tables because the
        // physical frames allocated by map_pages are NOT contiguous.
        for (uint32_t p = 0; p < page_count; p++) {
            uint32_t page_virt = virt_base + p * PMM_PAGE_SIZE;
            uint8_t* kptr = elf_resolve_virt(dir_phys, page_virt);
            if (!kptr) {
                klog_warn("elf_load: resolve failed for virt=%x", page_virt);
                paging_destroy_user_dir(dir_phys);
                return -1;
            }
            elf_memset(kptr, 0, PMM_PAGE_SIZE);
        }

        // Copy p_filesz bytes from the file into the correct virtual pages.
        if (ph->p_filesz > 0) {
            if (vfs_seek(fd, ph->p_offset, VFS_SEEK_SET) < 0) {
                klog_warn("elf_load: seek to segment offset %x failed",
                          ph->p_offset);
                paging_destroy_user_dir(dir_phys);
                return -1;
            }

            // write_virt tracks the current destination virtual byte address.
            // It starts at p_vaddr, which may not be page-aligned.
            uint32_t write_virt = ph->p_vaddr;
            uint32_t remaining  = ph->p_filesz;

            while (remaining > 0) {
                uint32_t page_offset = write_virt & (PMM_PAGE_SIZE - 1);
                uint32_t page_avail  = PMM_PAGE_SIZE - page_offset;
                uint32_t chunk       = remaining < page_avail ? remaining
                                                              : page_avail;

                // Fill page_buf with `chunk` bytes from the file.
                uint32_t buf_pos = 0;
                uint32_t to_read = chunk;
                while (to_read > 0) {
                    int n = vfs_read(fd, page_buf + buf_pos, to_read);
                    if (n <= 0) {
                        klog_warn("elf_load: short read on segment %u", i);
                        paging_destroy_user_dir(dir_phys);
                        return -1;
                    }
                    buf_pos += (uint32_t)n;
                    to_read -= (uint32_t)n;
                }

                // Resolve the destination physical page and copy.
                uint32_t page_base = write_virt & ~(PMM_PAGE_SIZE - 1);
                uint8_t* kptr = elf_resolve_virt(dir_phys, page_base);
                if (!kptr) {
                    klog_warn("elf_load: resolve failed writing virt=%x",
                              page_base);
                    paging_destroy_user_dir(dir_phys);
                    return -1;
                }
                elf_memcpy(kptr + page_offset, page_buf, chunk);

                write_virt += chunk;
                remaining  -= chunk;
            }
        }

        klog_info("ELF: loaded segment %u vaddr=%x memsz=%u filesz=%u flags=%x",
                  i, ph->p_vaddr, ph->p_memsz, ph->p_filesz, ph->p_flags);
    }

    // ---- 5. Return results --------------------------------------------------

    result->dir_phys = dir_phys;
    result->entry    = ehdr.e_entry;

    klog_info("ELF: loaded OK - entry=%x dir_phys=%x", ehdr.e_entry, dir_phys);
    return 0;
}