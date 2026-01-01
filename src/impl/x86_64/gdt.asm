global gdt_load

%include "src/assets/gdt_constants.inc"

gdt_load:
    mov ecx, 0xC0000101 ; IA32_GS_BASE
    rdmsr               ; Reads MSR into EDX:EAX
    push rdx            ; Push high 32 bits
    push rax            ; Push low 32 bits

    lgdt [rdi]

    mov ax, KDATA_SEL
    mov ds, ax
    mov es, ax
    mov ss, ax
    
    xor ax, ax
    mov fs, ax
    mov gs, ax

    pop rax             ; Pop low 32 bits
    pop rdx             ; Pop high 32 bits
    mov ecx, 0xC0000101
    wrmsr               ; Write EDX:EAX back to GS_BASE

    push KCODE_SEL
    lea rax, [rel .reload_cs]
    push rax
    retfq

.reload_cs:
    ret