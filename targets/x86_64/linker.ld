PHDRS
{
  boot  PT_LOAD FILEHDR PHDRS FLAGS(5); /* R + X */
  k_text PT_LOAD FLAGS(5); /* R + X */
  k_data PT_LOAD FLAGS(6); /* R + W */
}

SECTIONS
{
  . = 1M;

  .boot : {
    KEEP(*(.multiboot_header))
    *(.early_text .early_text.*)
    *(.early_rodata .early_rodata.*)
    *(.early_data .early_data.*)
    *(.early_bss .early_bss.*)
  } :boot

  . = 0xFFFFFFFF80000000;

  .text ALIGN(4096) : AT(ALIGN(ADDR(.boot) + SIZEOF(.boot))) {
      _text_start = .;
      *(.text .text.*)
      . = ALIGN(4096);
      _text_end = .;
  } :k_text
  _kernel_phys_start = LOADADDR(.text);

  .rodata ALIGN(4096) : AT(LOADADDR(.text) + SIZEOF(.text)) {
      _rodata_start = .;
      *(.rodata .rodata.*)
      . = ALIGN(4096);
      _rodata_end = .;
  } :k_text

  .data ALIGN(4096) : AT(LOADADDR(.rodata) + SIZEOF(.rodata)) {
      _data_start = .;
      *(.data .data.*)
      . = ALIGN(4096);
      _data_end = .;
  } :k_data

  .bss ALIGN(4096) (NOLOAD) : {
      _bss_start = .;
      *(.bss .bss.*)
      *(COMMON)
      . = ALIGN(4096);
      _bss_end = .;
  } :k_data

  _kernel_phys_end = LOADADDR(.bss) + SIZEOF(.bss);
}