global gdt_load
gdt_load:
    mov rax, rdi        ; rdi = pointer to GDTPtr
    lgdt [rax]          ; load GDT

    ; Reload data segment registers
    mov ax, 0x10        ; kernel data selector
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Far jump via memory descriptor
    ; 6-byte descriptor: [offset (4 bytes), selector (2 bytes)]
    jmp [rel gdt_jump_ptr]

gdt_jump_ptr:
    dq reload_cs        ; RIP (64-bit)
    dw 0x08             ; CS (selector)

reload_cs:
    ret                 ; return to caller