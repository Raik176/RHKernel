ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5); /* PF_R | PF_X */
    rodata PT_LOAD FLAGS(4); /* PF_R */
    data   PT_LOAD FLAGS(6); /* PF_R | PF_W */
}

SECTIONS
{
    . = 0x400000;

    . = ALIGN(0x1000);
    .text : {
        *(.text .text.*)
        *(.gnu.linkonce.t.*)
    } :text

    . = ALIGN(0x1000);
    .rodata : {
        *(.rodata .rodata.*)
        *(.gnu.linkonce.r.*)
        *(.eh_frame*)
    } :rodata

    . = ALIGN(0x1000);
    .data : {
        *(.data .data.*)
        *(.gnu.linkonce.d.*)
        *(.got .got.*)
        *(.got.plt)
        *(.init_array .init_array.*)
        *(.fini_array .fini_array.*)
    } :data

    . = ALIGN(0x1000);
    .bss : {
        *(COMMON)
        *(.bss .bss.*)
        *(.gnu.linkonce.b.*)
    } :data

    /DISCARD/ : {
        *(.note.GNU-stack)
        *(.comment)
    }
}