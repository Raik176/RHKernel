ENTRY(_start)

SECTIONS
{
    /* Standard base for x86_64 ELF executables */
    . = 0x400000;

    .text : {
        *(.text)
    }

    .rodata : {
        *(.rodata)
    }

    .data : {
        *(.data)
    }

    .bss : {
        *(.bss)
        *(COMMON)
    }
}