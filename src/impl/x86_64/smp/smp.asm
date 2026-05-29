global trampoline_start
global trampoline_end
global ap_data_start
global ap_start_32
global ap_start_64
global gdt32
global gdt64
global ap_jump32_offset
global ap_gdt32_base
global ap_jump64_offset
global ap_gdt64_base

%include "src/assets/gdt_constants.inc"

section .text
default abs
trampoline_start:
[bits 16]
ap_start_16:
    cli
    cld

    mov ax, cs
    mov ds, ax

    movzx edi, ax
    shl edi, 4

    xor ax, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    lgdt [ptr_gdt32 - trampoline_start]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp dword far [jump32_ptr - trampoline_start]

[bits 32]
ap_start_32:
    mov ax, KDATA_SEL
    mov ds, ax
    mov ss, ax

    mov eax, cr4
    or eax, 1 << 5
    test dword [edi + data_flags - trampoline_start], 1
    jz .write_cr4
    or eax, 1 << 12
.write_cr4:
    mov cr4, eax

    mov eax, [edi + data_cr3 - trampoline_start]
    mov cr3, eax

    xor esi, esi
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .write_efer

    mov eax, 0x80000001
    cpuid
    test edx, 1 << 20
    jz .write_efer
    mov esi, 1 << 11

.write_efer:
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    or eax, esi
    wrmsr

    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax

    lgdt [edi + ptr_gdt64 - trampoline_start]

    jmp far [edi + jump64_ptr - trampoline_start]

[bits 64]
ap_start_64:
    mov ax, KDATA_SEL
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov rsp, [rdi + data_stack - trampoline_start]
    mov rbp, 0

    mov rax, [rdi + data_entry - trampoline_start]
    call rax

.halt:
    hlt
    jmp .halt

[bits 16]
align 8
jump32_ptr:
ap_jump32_offset: dd 0
    dw KCODE_SEL

align 16
gdt32:
    dq 0x0000000000000000
    dq 0x00cf9a000000ffff
    dq 0x00cf92000000ffff
ptr_gdt32:
    dw 23
ap_gdt32_base: dd 0

[bits 32]
align 8
jump64_ptr:
ap_jump64_offset: dd 0
    dw KCODE_SEL

align 16
gdt64:
    dq 0x0000000000000000
    dq 0x00af9a000000ffff
    dq 0x0000920000000000
ptr_gdt64:
    dw 23
ap_gdt64_base: dq 0

[bits 16]
align 8
ap_data_start:
data_cr3:           dd 0
data_flags:         dd 0
data_cpu_index:     dq 0
data_stack:         dq 0
data_entry:         dq 0
data_status:        dq 0
data_cpu_local_ptr: dq 0
data_lapic_id:      dd 0
trampoline_end:
