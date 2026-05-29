global start
global pml5_table
global pml5_table_end
global pml4_table
global pml4_table_end
global pdp_table
global pdp_table_end
global page_directory
global page_directory_end
global paging_mode_la57
global paging_phys_map_base
global paging_phys_direct_map_size
global paging_user_top
extern long_mode_start

section .early_text
bits 32

%include "src/assets/gdt_constants.inc"

%define CPUID_BASIC_FEATURES     0x00000001
%define CPUID_EXTENDED_MAX       0x80000000
%define CPUID_EXTENDED_FEATURES  0x80000001
%define CPUID_STRUCTURED_FEATURES 0x00000007

%define FEAT_EDX_MSR             (1 << 5)
%define FEAT_EDX_PAE             (1 << 6)
%define FEAT_EDX_APIC            (1 << 9)
%define FEAT_EDX_PSE             (1 << 3)
%define FEAT_EDX_FXSR            (1 << 24)
%define FEAT_EDX_SSE             (1 << 25)
%define FEAT_EDX_SSE2            (1 << 26)
%define EXT_EDX_LONG_MODE        (1 << 29)
%define FEAT7_ECX_LA57            (1 << 16)

start:
    mov esp, stack_top

    call serial_init

    call check_cpuid
    call check_x86_64_baseline
    call setup_page_tables
    call enable_paging

    lgdt [gdt64_ptr]

    jmp KCODE_SEL:long_mode_start

setup_page_tables:
    cmp byte [boot_la57_supported], 0
    je .four_level

    mov eax, pml4_table
    or eax, 0b11
    mov [pml5_table + 0*8], eax
    mov [pml5_table + 511*8], eax

    mov dword [paging_mode_la57], 1
    mov dword [paging_mode_la57 + 4], 0
    mov dword [paging_phys_map_base], 0x00000000
    mov dword [paging_phys_map_base + 4], 0xFF000000
    mov dword [paging_phys_direct_map_size], 0x00000000
    mov dword [paging_phys_direct_map_size + 4], 0x00FF8000
    mov dword [paging_user_top], 0x00000000
    mov dword [paging_user_top + 4], 0x01000000
    jmp .link_pml4

.four_level:
    mov eax, pdp_table
    or eax, 0b11
    mov [pml4_table + 0*8], eax

.link_pml4:
    mov eax, pdp_table
    or eax, 0b11
    mov [pml4_table + 0*8], eax

    mov eax, page_directory
    or eax, 0b11
    mov [pdp_table + 0*8], eax

    mov eax, 0
    or eax, 0b10000011      ; Present | Writable | 2 MiB page
    mov [page_directory + 0*8], eax

    ret

enable_paging:
    mov eax, pml4_table
    cmp byte [boot_la57_supported], 0
    je .set_cr3
    mov eax, pml5_table
.set_cr3:
    mov cr3, eax

    mov eax, cr4
    or eax, 1 << 5          ; PAE
    cmp byte [boot_la57_supported], 0
    je .write_cr4
    or eax, 1 << 12         ; LA57
.write_cr4:
    mov cr4, eax

    mov ecx, 0xC0000080     ; IA32_EFER
    rdmsr
    or eax, 1 << 8          ; LME
    wrmsr

    mov eax, cr0
    or eax, 1 << 31         ; PG
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
    xor eax, ecx
    test eax, 1 << 21
    jz .no_cpuid
    ret
.no_cpuid:
    mov esi, msg_no_cpuid
    jmp boot_fail

check_x86_64_baseline:
    push ebx
    mov eax, 0
    cpuid
    mov edi, eax
    cmp edi, CPUID_BASIC_FEATURES
    jb .no_basic_leaf

    mov byte [boot_la57_supported], 0

    mov eax, CPUID_BASIC_FEATURES
    cpuid
    test edx, FEAT_EDX_MSR
    jz .no_msr
    test edx, FEAT_EDX_PAE
    jz .no_pae
    test edx, FEAT_EDX_PSE
    jz .no_pse
    test edx, FEAT_EDX_FXSR
    jz .no_fxsr
    test edx, FEAT_EDX_SSE
    jz .no_sse
    test edx, FEAT_EDX_SSE2
    jz .no_sse2
    test edx, FEAT_EDX_APIC
    jz .no_apic

    cmp edi, CPUID_STRUCTURED_FEATURES
    jb .skip_la57
    mov eax, CPUID_STRUCTURED_FEATURES
    xor ecx, ecx
    cpuid
    test ecx, FEAT7_ECX_LA57
    jz .skip_la57
    mov byte [boot_la57_supported], 1
.skip_la57:

    mov eax, CPUID_EXTENDED_MAX
    cpuid
    cmp eax, CPUID_EXTENDED_FEATURES
    jb .no_ext_leaf

    mov eax, CPUID_EXTENDED_FEATURES
    cpuid
    test edx, EXT_EDX_LONG_MODE
    jz .no_long_mode
    pop ebx
    ret

.no_basic_leaf:
    mov esi, msg_no_basic_leaf
    jmp boot_fail
.no_msr:
    mov esi, msg_no_msr
    jmp boot_fail
.no_pae:
    mov esi, msg_no_pae
    jmp boot_fail
.no_pse:
    mov esi, msg_no_pse
    jmp boot_fail
.no_fxsr:
    mov esi, msg_no_fxsr
    jmp boot_fail
.no_sse:
    mov esi, msg_no_sse
    jmp boot_fail
.no_sse2:
    mov esi, msg_no_sse2
    jmp boot_fail
.no_apic:
    mov esi, msg_no_apic
    jmp boot_fail
.no_ext_leaf:
    mov esi, msg_no_ext_leaf
    jmp boot_fail
.no_long_mode:
    mov esi, msg_no_long_mode
    jmp boot_fail

serial_init:
    mov dx, 0x3F8 + 1
    xor al, al
    out dx, al
    mov dx, 0x3F8 + 3
    mov al, 0x80
    out dx, al
    mov dx, 0x3F8 + 0
    mov al, 0x03
    out dx, al
    mov dx, 0x3F8 + 1
    xor al, al
    out dx, al
    mov dx, 0x3F8 + 3
    mov al, 0x03
    out dx, al
    mov dx, 0x3F8 + 2
    mov al, 0xC7
    out dx, al
    mov dx, 0x3F8 + 4
    mov al, 0x0B
    out dx, al
    ret

serial_putchar:
    push eax
    push edx
.wait:
    mov dx, 0x3F8 + 5
    in al, dx
    test al, 0x20
    jz .wait
    mov dx, 0x3F8
    mov al, bl
    out dx, al
    pop edx
    pop eax
    ret

boot_fail:
    ; ESI points to a zero terminated reason
    push esi
    mov edi, 0xB8000
    mov ah, 0x4F
    mov esi, msg_boot_prefix
.print_prefix:
    lodsb
    test al, al
    jz .print_reason_setup
    mov bl, al
    call serial_putchar
    stosw
    jmp .print_prefix
.print_reason_setup:
    pop esi
.print_reason:
    lodsb
    test al, al
    jz .newline
    mov bl, al
    call serial_putchar
    stosw
    jmp .print_reason
.newline:
    mov bl, 13
    call serial_putchar
    mov bl, 10
    call serial_putchar
.hang:
    cli
    hlt
    jmp .hang

section .early_bss
align 4096
pml5_table:
    resb 4096
pml5_table_end:
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
    resb 4096
stack_top:

section .early_data
align 8
boot_la57_supported: db 0
align 8
paging_mode_la57: dq 0
paging_phys_map_base: dq 0xFFFF800000000000
paging_phys_direct_map_size: dq 0x00007F0000000000
paging_user_top: dq 0x0000800000000000

; TODO: is this really a good idea..?
section .early_rodata
msg_boot_prefix: db 'BOOT FAIL: ', 0
msg_no_cpuid: db 'CPUID not supported', 0
msg_no_basic_leaf: db 'CPUID leaf 1 unavailable', 0
msg_no_msr: db 'MSR instructions unavailable', 0
msg_no_pae: db 'PAE unavailable', 0
msg_no_pse: db '2 MiB pages unavailable', 0
msg_no_fxsr: db 'FXSAVE/FXRSTOR unavailable', 0
msg_no_sse: db 'SSE unavailable', 0
msg_no_sse2: db 'SSE2 unavailable', 0
msg_no_apic: db 'local APIC unavailable', 0
msg_no_ext_leaf: db 'extended CPUID leaf unavailable', 0
msg_no_long_mode: db 'long mode unavailable', 0

gdt64:
    dq 0                             ; Null
    dq 0x00209A0000000000            ; Code segment, 64-bit, present
    dq 0x0000920000000000            ; Data segment, present, writable
gdt64_ptr:
    dw gdt64_end - gdt64 - 1
    dq gdt64
gdt64_end:
