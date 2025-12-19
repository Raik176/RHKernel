PHDRS
{
  boot PT_LOAD FLAGS(5);   /* R + X */
  kernel PT_LOAD FLAGS(7); /* R + W + X */
}

SECTIONS
{
  . = 1M;

  .boot : {
    *(.multiboot_header)
  } :boot

  .early_text : {
    *(.early_text)
    *(.early_text.*)
  } :boot

  .early_rodata : {
    *(.early_rodata)
    *(.early_rodata.*)
  } :boot

  .early_data : {
    *(.early_data)
    *(.early_data.*)
  } :boot

  .early_bss (NOLOAD) : {
    *(.early_bss)
    *(.early_bss.*)
  } :boot

  . = 0xFFFFFFFF80000000;
  _kernel_phys_start = LOADADDR(.text);

  .text : AT(ALIGN(ADDR(.early_bss) + SIZEOF(.early_bss))) {
      *(.text)
      *(.text.*)
  } :kernel

  .rodata : AT(LOADADDR(.text) + SIZEOF(.text)) {
    *(.rodata)
    *(.rodata.*)
  } :kernel

  .data : AT(LOADADDR(.rodata) + SIZEOF(.rodata)) {
    *(.data)
    *(.data.*)
  } :kernel

  .bss (NOLOAD) : {
    *(.bss)
    *(.bss.*)
    *(COMMON)
    . = ALIGN(4096);
  } :kernel

  _kernel_phys_end = LOADADDR(.bss) + SIZEOF(.bss);
}
