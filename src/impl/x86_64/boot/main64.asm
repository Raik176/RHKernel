global long_mode_start
global phys_map_pdp_table
global phys_map_pd_table
global high_pdp_table
global high_pd_table
global higher_stack_top
extern kmain
extern pml4_table
extern page_directory
extern _kernel_phys_start
extern enable_cpu_features

[bits 64]
section .text
trampoline:
    call enable_cpu_features
    mov rdi, rbx                 ; multiboot pointer
    mov rax, kmain
    jmp rax

section .early_text
long_mode_start:
    mov ax, 0
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov rsp, stack_top

    call setup_higher_half_mapping
    call setup_direct_physical_mapping

    mov rax, cr3
    mov cr3, rax

    mov rsp, higher_stack_top

    mov rax, trampoline
    jmp rax

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
    resb 1024 * 8
higher_stack_top:
section .early_bss
align 16
stack_bottom:
    resb 1024 * 2
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