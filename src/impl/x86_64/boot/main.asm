global start
global pml4_table
global pml4_table_end
global pdp_table
global pdp_table_end
global page_directory
global page_directory_end
extern long_mode_start

section .early_text
bits 32

%include "src/assets/gdt_constants.inc"

start:
    mov esp, stack_top
    
    call check_cpuid
    call setup_page_tables
    call enable_paging

    lgdt [gdt64_ptr]

    jmp KCODE_SEL:long_mode_start  

setup_page_tables:    
    mov eax, pdp_table
    or eax, 0b11
    mov [pml4_table + 0*8], eax

    mov eax, page_directory
    or eax, 0b11
    mov [pdp_table + 0*8], eax

    mov eax, 0
    or eax, 0b10000011      ; Present | Writable | Huge Page
    mov [page_directory + 0*8], eax

    ret

enable_paging:
    mov eax, pml4_table
    mov cr3, eax

    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax
    ret

check_cpuid:
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    cmp eax, ecx
    je .no_cpuid
    ret
.no_cpuid:
    mov dword [0xb8000], 0x4f454f4e
    hlt

section .early_bss
align 4096
pml4_table:
    resb 4096
pml4_table_end:
align 4096
pdp_table:
    resb 4096
pdp_table_end:
align 4096
page_directory:
    resb 4096
page_directory_end:
stack_bottom:
    resb 512
stack_top:

section .early_rodata
gdt64:
    dq 0                             ; Null
    dq 0x00209A0000000000            ; Code segment, 64-bit, present
    dq 0x0000920000000000            ; Data segment, present, writable
gdt64_ptr:
    dw gdt64_end - gdt64 - 1
    dq gdt64
gdt64_end: