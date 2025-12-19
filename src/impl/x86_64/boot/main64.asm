global long_mode_start
extern kmain
extern pml4_table
extern page_directory

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

    call setup_higher_half_mapping

    mov rsp, virt_stack_top

    mov rdi, rbx                 ; multiboot pointer
    mov rax, kmain
    jmp rax

setup_higher_half_mapping:
    mov rax, high_pdp_table
    or rax, 0x03
    mov [pml4_table + 511*8], rax

    mov rax, high_pd_table
    or rax, 0x03
    mov [high_pdp_table + 510*8], rax

    mov rax, 0
    or rax, 0b10000011 ; Present | Writable | Huge (2MB)
    mov [high_pd_table + 0*8], rax

    mov rax, cr3
    mov cr3, rax
    ret

section .bss
align 16
virt_stack_bottom:
    resb 4096 * 4
virt_stack_top:
section .early_bss
align 16
stack_bottom:
    resb 4096 * 4
stack_top:
align 4096
high_pdp_table:
    resb 4096
align 4096
high_pd_table:
    resb 4096