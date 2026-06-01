#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// ELF32 loader — Milestone 9
//
// Parses a statically-linked ELF32 executable from the VFS and loads its
// PT_LOAD segments into a fresh user address space.
//
// Only ET_EXEC (executable) files targeting EM_386 (x86 32-bit) are accepted.
// Dynamically linked objects and shared libraries are not supported.
// ---------------------------------------------------------------------------

// ELF identification
#define ELF_MAGIC       0x464C457Fu  // "\x7FELF" as little-endian uint32
#define ELF_CLASS_32    1
#define ELF_DATA_LSB    1            // little-endian (ELFDATA2LSB)
#define ELF_TYPE_EXEC   2
#define ELF_MACHINE_386 3

// Segment types
#define PT_NULL    0
#define PT_LOAD    1

// Segment flags (p_flags)
#define PF_X  (1u << 0)             // execute
#define PF_W  (1u << 1)             // write
#define PF_R  (1u << 2)             // read

// ELF32 file header (52 bytes)
typedef struct {
    uint8_t  e_ident[16];           // magic, class, data, version, OS/ABI, padding
    uint16_t e_type;                // ET_EXEC = 2
    uint16_t e_machine;             // EM_386  = 3
    uint32_t e_version;             // 1
    uint32_t e_entry;               // virtual entry point
    uint32_t e_phoff;               // offset of program header table
    uint32_t e_shoff;               // offset of section header table (unused here)
    uint32_t e_flags;               // architecture flags (0 for x86)
    uint16_t e_ehsize;              // size of this header (52)
    uint16_t e_phentsize;           // size of one program header entry (32)
    uint16_t e_phnum;               // number of program header entries
    uint16_t e_shentsize;           // size of one section header entry
    uint16_t e_shnum;               // number of section header entries
    uint16_t e_shstrndx;            // section name string table index
} __attribute__((packed)) elf32_ehdr_t;

// ELF32 program header (32 bytes)
typedef struct {
    uint32_t p_type;                // segment type (PT_LOAD etc.)
    uint32_t p_offset;              // offset in file
    uint32_t p_vaddr;               // virtual address to load at
    uint32_t p_paddr;               // physical address (ignored for user ELFs)
    uint32_t p_filesz;              // bytes in file image
    uint32_t p_memsz;               // bytes in memory (>= p_filesz; tail is BSS)
    uint32_t p_flags;               // PF_R / PF_W / PF_X
    uint32_t p_align;               // alignment (must be power of two)
} __attribute__((packed)) elf32_phdr_t;

// Result returned by elf_load on success.
typedef struct elf_load_result {
    uint32_t dir_phys;              // physical address of the new page directory
    uint32_t entry;                 // virtual entry point (e_entry)
} elf_load_result_t;

// Load an ELF32 executable from the open file descriptor `fd`.
//
// On success:
//   - Allocates a fresh user page directory (via paging_create_user_dir).
//   - Maps all PT_LOAD segments into it.
//   - Fills `*result` with the page directory physical address and entry point.
//   - Returns 0.
//
// On failure:
//   - Frees any partially-allocated resources.
//   - Returns a negative VFS error code or -1 for a malformed ELF.
int elf_load(int fd, elf_load_result_t* result);