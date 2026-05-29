ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5); /* PF_R | PF_X */
    rodata PT_LOAD FLAGS(4); /* PF_R */
    eh_frame_hdr PT_GNU_EH_FRAME FLAGS(4);
    data   PT_LOAD FLAGS(6); /* PF_R | PF_W */
    dynamic PT_DYNAMIC FLAGS(6);
}

SECTIONS
{
    . = 0;

    . = ALIGN(0x1000);
    .text : {
        *(.text .text.*)
        *(.gnu.linkonce.t.*)
    } :text

    . = ALIGN(0x1000);
    .rodata : {
        *(.rodata .rodata.*)
        *(.gnu.linkonce.r.*)
    } :rodata

    .eh_frame_hdr : {
        *(.eh_frame_hdr)
    } :rodata :eh_frame_hdr

    .eh_frame : {
        PROVIDE_HIDDEN(__eh_frame_start = .);
        KEEP(*(.eh_frame))
        KEEP(*(.eh_frame.*))
        PROVIDE_HIDDEN(__eh_frame_end = .);
    } :rodata

    .gcc_except_table : {
        KEEP(*(.gcc_except_table .gcc_except_table.*))
    } :rodata

    .interp : { *(.interp) } :rodata
    .dynsym : { *(.dynsym) } :rodata
    .dynstr : { *(.dynstr) } :rodata
    .hash : { *(.hash) } :rodata
    .gnu.hash : { *(.gnu.hash) } :rodata
    .rela.dyn : { *(.rela.dyn) } :rodata
    .rela.plt : { *(.rela.plt) } :rodata

    . = ALIGN(0x1000);
    .data : {
        *(.data .data.*)
        *(.gnu.linkonce.d.*)
        *(.got .got.*)
        *(.got.plt)
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.preinit_array.*)))
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN(__preinit_array_end = .);
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        KEEP(*(SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.ctors))
        PROVIDE_HIDDEN(__init_array_end = .);
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP(*(.fini_array))
        KEEP(*(SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP(*(.dtors))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } :data

    .dynamic : {
        *(.dynamic)
    } :data :dynamic

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
