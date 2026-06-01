global idt_load

idt_load:
    mov eax, [esp + 4]
    lidt [eax]
    ret

; Mark the stack as non-executable — suppresses GNU linker warning.
section .note.GNU-stack noalloc noexec nowrite progbits