global enable_cpu_features
extern syscall_entry

[bits 64]
section .text

IA32_EFER        equ 0xC0000080
IA32_STAR        equ 0xC0000081
IA32_LSTAR       equ 0xC0000082
IA32_SFMASK      equ 0xC0000084

enable_cpu_features:
    call enable_nx
    call enable_sse
    call enable_wp
    call enable_fpu
    call enable_syscall
    call enable_ne
    ret

enable_syscall:
    ; 1. Enable SCE (System Call Extensions) in EFER
    mov ecx, IA32_EFER
    rdmsr
    or eax, 1
    wrmsr

    ; 2. Set the Entry Point (LSTAR)
    mov ecx, IA32_LSTAR
    mov rax, syscall_entry       ; This is the label we'll define in step 2
    mov rdx, rax
    shr rdx, 32                  ; High 32 bits in EDX, Low in EAX
    wrmsr

    ; 3. Set Segments (STAR)
    ; Kernel segments: Base 0x08 (Code), 0x10 (Data)
    ; User segments: Base 0x1B (for sysret compatibility)
    ; Bits 32-47: Kernel CS/SS (0x08)
    ; Bits 48-63: User CS/SS (0x13 or 0x1B depending on GDT layout)
    mov ecx, IA32_STAR
    xor eax, eax
    mov edx, 0x001B0008
    wrmsr

    ; 4. Set SFMASK (What flags to clear on syscall entry)
    ; We usually want to disable interrupts (IF bit 9)
    mov ecx, IA32_SFMASK
    mov eax, 0x200               ; Clear IF bit
    wrmsr
    ret

enable_wp: ; Write Protect
    mov rax, cr0
    or rax, 1 << 16
    mov cr0, rax

enable_fpu: ; Floating point unit
    mov rax, cr0
    and rax, ~((1 << 2) | (1 << 3))
    mov cr0, rax

enable_nx:
    mov ecx, 0xC0000080    ; IA32_EFER MSR
    rdmsr                   ; EDX:EAX = MSR value
    or eax, 1 << 11         ; Set NXE bit (bit 11)
    wrmsr
    ret

enable_sse:
    ; --- CR0 ---
    mov rax, cr0
    and rax, 0xFFFFFFFFFFFFFFFB ; Clear CR0.EM (bit 2)
    or  rax, 0x2                ; Set CR0.MP (bit 1)
    mov cr0, rax

    ; --- CR4 ---
    mov rax, cr4
    or  rax, (1 << 9) | (1 << 10)  ; Set CR4.OSFXSR (bit 9) and CR4.OSXMMEXCPT (bit 10)
    mov cr4, rax

    ret

enable_ne:
    mov rax, cr0
    or rax, 1 << 5          ; Set NE (bit 5)
    mov cr0, rax
    ret