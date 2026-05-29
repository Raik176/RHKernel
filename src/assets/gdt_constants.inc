KCODE_SEL  equ 0x08  ; Kernel code segment, RPL 0
KDATA_SEL  equ 0x10  ; Kernel data segment, RPL 0
UCODE32_SEL equ 0x18 | 3
UDATA32_SEL equ 0x20 | 3
UDATA64_SEL equ 0x28 | 3
UCODE64_SEL equ 0x30 | 3
TSS_SEL    equ 0x38  ; TSS selector (16 bytes, uses entries 7 & 8)