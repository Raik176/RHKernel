global enable_cpu_features
extern enable_syscalls

[bits 64]
section .text

%define CPUID_BASIC_FEATURES     0x00000001
%define CPUID_EXTENDED_MAX       0x80000000
%define CPUID_EXTENDED_FEATURES  0x80000001
%define EFER_MSR                 0xC0000080

%define EFER_NXE                 (1 << 11)
%define CPUID_EDX_NX             (1 << 20)
%define CPUID_EDX_SYSCALL        (1 << 11)
%define CPUID_EDX_PGE            (1 << 13)
%define CPUID_EDX_PAT            (1 << 16)
%define CPUID_ECX_XSAVE          (1 << 26)
%define CPUID_ECX_AVX            (1 << 28)
%define IA32_PAT_MSR             0x00000277
%define PAT_TYPE_WC              0x01
%define XCR0_X87_SSE_AVX         0x00000007

enable_cpu_features:
    push rbx
    call enable_fpu
    call enable_sse
    call enable_wp
    call enable_ne
    call enable_pat_if_supported
    call enable_pge_if_supported
    call enable_xsave_avx_if_supported
    call enable_nx_if_supported
    call enable_syscalls_if_supported
    pop rbx
    ret

enable_wp: ; Write Protect
    mov rax, cr0
    or rax, 1 << 16
    mov cr0, rax
    ret

enable_fpu: ; Floating point unit
    mov rax, cr0
    and rax, ~((1 << 2) | (1 << 3)) ; Clear EM and TS
    or  rax, 1 << 1                 ; Set MP
    mov cr0, rax
    fninit
    ret

enable_pge_if_supported:
    mov eax, CPUID_BASIC_FEATURES
    cpuid
    test edx, CPUID_EDX_PGE
    jz .done

    mov rax, cr4
    or rax, 1 << 7
    mov cr4, rax
.done:
    ret

enable_pat_if_supported:
    mov eax, CPUID_BASIC_FEATURES
    cpuid
    test edx, CPUID_EDX_PAT
    jz .done

    mov ecx, IA32_PAT_MSR
    rdmsr
    and edx, ~0xFF
    or  edx, PAT_TYPE_WC
    wrmsr
.done:
    ret

enable_xsave_avx_if_supported:
    mov eax, CPUID_BASIC_FEATURES
    cpuid
    test ecx, CPUID_ECX_XSAVE
    jz .done
    test ecx, CPUID_ECX_AVX
    jz .done

    mov rax, cr4
    or  rax, 1 << 18
    mov cr4, rax

    xor ecx, ecx
    mov eax, XCR0_X87_SSE_AVX
    xor edx, edx
    xsetbv
.done:
    ret

enable_nx_if_supported:
    mov eax, CPUID_EXTENDED_MAX
    cpuid
    cmp eax, CPUID_EXTENDED_FEATURES
    jb .done

    mov eax, CPUID_EXTENDED_FEATURES
    cpuid
    test edx, CPUID_EDX_NX
    jz .done

    mov ecx, EFER_MSR
    rdmsr
    or eax, EFER_NXE
    wrmsr
.done:
    ret

enable_sse:
    mov rax, cr0
    and rax, ~(1 << 2)              ; Clear CR0.EM
    or  rax,  (1 << 1)              ; Set CR0.MP
    mov cr0, rax

    mov rax, cr4
    or  rax, (1 << 9) | (1 << 10)   ; OSFXSR | OSXMMEXCPT
    mov cr4, rax
    ret

enable_ne:
    mov rax, cr0
    or rax, 1 << 5                  ; Native x87 error reporting
    mov cr0, rax
    ret

enable_syscalls_if_supported:
    mov eax, CPUID_EXTENDED_MAX
    cpuid
    cmp eax, CPUID_EXTENDED_FEATURES
    jb .done

    mov eax, CPUID_EXTENDED_FEATURES
    cpuid
    test edx, CPUID_EDX_SYSCALL
    jz .done

    call enable_syscalls
.done:
    ret
