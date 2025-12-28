global enable_cpu_features
extern enable_syscalls

[bits 64]
section .text

enable_cpu_features:
    call enable_nx
    call enable_sse
    call enable_wp
    call enable_fpu
    call enable_ne
    call enable_syscalls
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