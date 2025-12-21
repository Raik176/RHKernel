section .multiboot_header
header_start:
    ; 1. Magic number
    dd 0xe85250d6 ; Multiboot2 magic
    ; 2. Architecture
    dd 0            ; Protected Mode i386
    ; 3. Header length
    dd header_end - header_start
    ; 4. Checksum
    dd 0x100000000 - (0xe85250d6 + 0 + (header_end - header_start))

    ; --- Information Request Tag (Type 1) ---
    .info_request_tag:
        dw 1            ; Type = 1 (Information Request)
        dw 0            ; Flags = 0 (Required)
        dd .end_of_info_request - .info_request_tag
        
        dd 6            ; Request Tag Type 6: Memory Map Tag (MMAP)
    
    .end_of_info_request:

    align 8

    .framebuffer_tag:
        dw 5              ; Type = 5 (Framebuffer)
        dw 0              ; Flags = 0 (optional, but usually 0)
        dd .framebuffer_tag_end - .framebuffer_tag

        dd 0              ; Width  = 0 (no preference)
        dd 0              ; Height = 0 (no preference)
        dd 0              ; Depth  = 0 (no preference)

    .framebuffer_tag_end:

    align 8
    
    ; --- End Tag ---
    dw 0            ; Type = 0
    dw 0            ; Flags = 0
    dd 8            ; Size = 8
header_end: