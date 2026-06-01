/* Linker script for user-space ELF32 programs running on MyOS.
 *
 * Load address: 0x400000 (4 MiB) — well above the kernel's identity-mapped
 * region (which ends around 1–2 MiB depending on kernel size) and below the
 * user stack at 0xBFFFF000.
 *
 * The kernel copies kernel PDEs into every user page directory, so virtual
 * addresses below PDE_KERNEL_END (the kernel region) are already occupied.
 * 0x400000 sits in PDE index 1 (covers 4–8 MiB), safely above the kernel.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x400000;

    .text   : { *(.text*)   }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*)   }
    .bss    :
    {
        *(COMMON)
        *(.bss*)
    }
}
