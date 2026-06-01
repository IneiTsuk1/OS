section .multiboot
align 8

MB2_MAGIC   equ 0xE85250D6
MB2_ARCH    equ 0
MB2_LENGTH  equ header_end - header_start
MB2_CHECKSUM equ -(MB2_MAGIC + MB2_ARCH + MB2_LENGTH)

header_start:
    dd MB2_MAGIC
    dd MB2_ARCH
    dd MB2_LENGTH
    dd MB2_CHECKSUM

    dw 0
    dw 0
    dd 8
header_end:

; ---------------------------------------------------------------------------
; GDT
;
; Slot  Selector  Description
;  0    0x00      Null
;  1    0x08      Kernel code  DPL=0
;  2    0x10      Kernel data  DPL=0
;  3    0x18      TSS          — zeroed here, filled at runtime by tss_init()
;  4    0x20      User code    DPL=3
;  5    0x28      User data    DPL=3
;
; All non-TSS segments are flat (base=0, limit=4 GiB).
; ---------------------------------------------------------------------------
section .data
align 8

global gdt_start
gdt_start:
    dq 0                ; 0x00 null

    ; 0x08 kernel code — DPL=0, execute/read, 32-bit, 4K granularity
    dw 0xFFFF           ; limit[15:0]
    dw 0x0000           ; base[15:0]
    db 0x00             ; base[23:16]
    db 0x9A             ; access: present, DPL=0, code, execute/read
    db 0xCF             ; flags: 4K gran, 32-bit; limit[19:16]=0xF
    db 0x00             ; base[31:24]

    ; 0x10 kernel data — DPL=0, read/write
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x92             ; access: present, DPL=0, data, read/write
    db 0xCF
    db 0x00

    ; 0x18 TSS — zeroed, filled at runtime by tss_init()
    dq 0

    ; 0x20 user code — DPL=3, execute/read, 32-bit, 4K granularity
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0xFA             ; access: present, DPL=3, code, execute/read
    db 0xCF
    db 0x00

    ; 0x28 user data — DPL=3, read/write
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0xF2             ; access: present, DPL=3, data, read/write
    db 0xCF
    db 0x00

gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

KERNEL_CODE equ 0x08
KERNEL_DATA equ 0x10
USER_CODE   equ 0x20
USER_DATA   equ 0x28

;----------------------------------------------------------------------------
; Kernel entry point and stack setup
;----------------------------------------------------------------------------
section .bss
align 16
stack_bottom:
    resb 16384  ; 16 KiB kernel stack

global stack_top
stack_top:

section .text
global _start
extern kernel_main

_start:
    mov edi, ebx        ; save MB2 info pointer before anything touches regs

    mov esp, stack_top
    cli

    lgdt [gdt_descriptor]

    mov ax, KERNEL_DATA
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    jmp KERNEL_CODE:.flush_cs
.flush_cs:

    push edi
    call kernel_main

.hang:
    hlt
    jmp .hang

section .note.GNU-stack noalloc noexec nowrite progbits