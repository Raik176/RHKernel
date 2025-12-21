section .rodata
global font_bitmap_start
global font_bitmap_end

font_bitmap_start:
    incbin "build/assets/font.bin"
font_bitmap_end: