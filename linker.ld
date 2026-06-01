ENTRY(_start)

PHDRS
{
    text    PT_LOAD FLAGS(5);   /* R-X */
    data    PT_LOAD FLAGS(6);   /* RW- */
}

SECTIONS
{
    . = 1M;

    .multiboot : { *(.multiboot) } :text
    .text      : { *(.text*)    } :text
    .rodata    : { *(.rodata*)  } :text

    kernel_text_end = .;    /* end of read-only/executable region — used by paging_init
                               to map .text+.rodata PAGE_USER (ring-3 can execute
                               kernel-resident user_task code) while keeping .data/.bss
                               kernel-private (no PAGE_USER). */

    .data      : { *(.data*)    } :data

    .bss :
    {
        *(COMMON)
        *(.bss*)
    } :data

    kernel_end = .;
}