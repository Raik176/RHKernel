global trampoline_start
global trampoline_end
global ap_data_start

%include "src/assets/gdt_constants.inc"

section .text
default abs
trampoline_start:
[bits 16]
ap_start_16:
    cli
    cld

    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    lgdt [ptr_gdt32 - trampoline_start + 0x8000]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp dword KCODE_SEL:(ap_start_32 - trampoline_start + 0x8000)

[bits 32]
ap_start_32:
    mov ax, KDATA_SEL
    mov ds, ax
    mov ss, ax

    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    mov eax, [data_cr3 - trampoline_start + 0x8000]
    mov cr3, eax

    mov ecx, 0xC0000080         ; EFER MSR
    rdmsr
    or eax, 1 << 8              ; LME bit
    wrmsr

    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax

    lgdt [ptr_gdt64 - trampoline_start + 0x8000]

    jmp KCODE_SEL:(ap_start_64 - trampoline_start + 0x8000)

[bits 64]
ap_start_64:
    mov ax, KDATA_SEL
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov rsp, [data_stack - trampoline_start + 0x8000]
    mov rbp, 0

    mov rax, [data_entry - trampoline_start + 0x8000]
    call rax

.halt:
    hlt
    jmp .halt

[bits 16]
align 16
gdt32:
    dq 0x0000000000000000       ; Null
    dq 0x00cf9a000000ffff       ; Code32 (0x08)
    dq 0x00cf92000000ffff       ; Data32 (0x10)
ptr_gdt32:
    dw 23
    dd (gdt32 - trampoline_start + 0x8000)

[bits 32]
align 16
gdt64:
    dq 0x0000000000000000       ; Null
    dq 0x00af9a000000ffff       ; Code64 (0x08)
    dq 0x00af92000000ffff       ; Data64 (0x10)
ptr_gdt64:
    dw 23
    dq (gdt64 - trampoline_start + 0x8000)

[bits 16]
align 8
ap_data_start:
data_cr3:       dd 0       ; Physical address of PML4
data_cpu_index: dq 0       ; Incremented ID unique to this core
data_stack:     dq 0       ; Virtual address of allocated stack
data_entry:     dq 0       ; Virtual address of ap_kernel_entry
data_status:    dq 0       ; Whether AP has initialized correctly
trampoline_end: