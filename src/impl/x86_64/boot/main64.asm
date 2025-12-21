global long_mode_start
global phys_map_pdp_table
global phys_map_pd_table
global high_pdp_table
global high_pd_table
extern kmain
extern pml4_table
extern page_directory
extern _kernel_phys_start

section .early_text
bits 64
long_mode_start:
    mov ax, 0
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov rsp, stack_top

    call enable_sse
    call enable_nx
    call setup_higher_half_mapping
    call setup_direct_physical_mapping

    mov rax, cr3
    mov cr3, rax

    mov rsp, higher_stack_top

    mov rdi, rbx                 ; multiboot pointer
    mov rax, kmain
    jmp rax

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

setup_direct_physical_mapping:
    ; Link PML4 entry 272 to our PDP table
    mov rax, phys_map_pdp_table
    or rax, 0x03                ; Present | Writable
    mov [pml4_table + 272 * 8], rax

    xor rcx, rcx                ; Counter (0 to 511)
.loop:
    mov rax, rcx
    shl rax, 30                 ; rcx * 1GB (2^30)
    or rax, 0x83                ; Present | Writable | Huge Page (Bit 7)
    mov [phys_map_pdp_table + rcx * 8], rax

    inc rcx
    cmp rcx, 512
    jne .loop

    ret

setup_higher_half_mapping:
    mov rax, high_pdp_table
    or rax, 0x03
    mov [pml4_table + 511*8], rax

    mov rax, high_pd_table
    or rax, 0x03
    mov [high_pdp_table + 510*8], rax

    mov rax, 0x00000000 | 0x83
    mov [high_pd_table + 0*8], rax
    
    mov rax, 0x00200000 | 0x83
    mov [high_pd_table + 1*8], rax
    
    mov rax, 0x00400000 | 0x83
    mov [high_pd_table + 2*8], rax

    ret

section .bss
align 16
higher_stack_bottom:
    resb 4096 * 4
higher_stack_top:
section .early_bss
align 16
stack_bottom:
    resb 4096 * 4
stack_top:
align 4096
phys_map_pdp_table:
    resb 4096
align 4096
phys_map_pd_table:
    resb 4096
align 4096
high_pdp_table:
    resb 4096
align 4096
high_pd_table:
    resb 4096