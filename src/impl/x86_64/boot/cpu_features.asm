global enable_cpu_features

[bits 64]
section .text

enable_cpu_features:
    call enable_nx
    call enable_sse
    ret

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