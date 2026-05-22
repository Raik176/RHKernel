global long_mode_start
global phys_map_pdp_table
global phys_map_pdp_table_end
global phys_map_pd_table
global phys_map_pd_table_end
global high_pdp_table
global high_pdp_table_end
global high_pd_table
global high_pd_table_end
global higher_stack_top
global higher_stack_bottom
extern kmain
extern pml4_table
extern page_directory
extern _kernel_phys_start
extern enable_cpu_features

[bits 64]
default rel
section .text
trampoline:
    call enable_cpu_features
    mov rdi, rbx                 ; multiboot pointer
    ; Enter C++ with the SysV AMD64 stack convention.
    ; RSP is 16-byte aligned here; CALL makes C++ function entry RSP % 16 == 8.
    ; A JMP leaves C++ with the wrong alignment and GCC may fault on movaps.
    mov rax, kmain
    call rax
.hang_after_kmain:
    hlt
    jmp .hang_after_kmain

section .early_text
long_mode_start:
    ; Canonicalize the 32-bit Multiboot info pointer passed in EBX.
    mov ebx, ebx

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

%define PHYS_MAP_GIB 64

setup_direct_physical_mapping:
    ; Link PML4 entry 256 to a PDPT for the kernel physical direct map.
    ;
    ; This is deliberately only a bootstrap map. PMM now defers usable spans
    ; above this range until after vmm::init() installs the full direct map, so
    ; the boot page tables no longer impose the kernel's maximum supported RAM.
    ; 64 GiB is kept here because it is plenty for early allocations while
    ; keeping .early_bss page-table storage small.
    mov rax, phys_map_pdp_table
    or rax, 0x03                 ; Present | Writable
    mov [pml4_table + 256 * 8], rax

    ; Each PD maps 1 GiB using 512 2 MiB entries.
    xor rcx, rcx
.link_pdp_loop:
    mov rax, phys_map_pd_table
    mov rdx, rcx
    shl rdx, 12                  ; rcx * 4096, one page directory per GiB
    add rax, rdx

    or rax, 0x03                 ; Present | Writable
    mov [phys_map_pdp_table + rcx * 8], rax

    inc rcx
    cmp rcx, PHYS_MAP_GIB
    jne .link_pdp_loop

    ; Fill PHYS_MAP_GIB page directories.
    xor rcx, rcx
.fill_pd_loop:
    mov rax, rcx
    shl rax, 21                  ; rcx * 2 MiB
    or rax, 0x83                 ; Present | Writable | Huge Page (2 MiB)

    mov [phys_map_pd_table + rcx * 8], rax

    inc rcx
    cmp rcx, PHYS_MAP_GIB * 512
    jne .fill_pd_loop

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
phys_map_pdp_table_end:
align 4096
phys_map_pd_table:
    resb 4096 * PHYS_MAP_GIB
phys_map_pd_table_end:
align 4096
high_pdp_table:
    resb 4096
high_pdp_table_end:
align 4096
high_pd_table:
    resb 4096
high_pd_table_end: