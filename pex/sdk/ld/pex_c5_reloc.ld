/* PEX linker script: target c5 (rv32imac), code model reloc.
 * Text starts at 0 with a nop so pex_main is not at address 0 (entry 0 means
 * "no entry"). .data follows the code with ALIGN(4), not ALIGN(8): the
 * loader copies one span and medany PC-relative refs need that distance.
 * Link with -mcmodel=medany -mno-relax -msmall-data-limit=0 --emit-relocs.
 */
ENTRY(pex_main)
PHDRS { text PT_LOAD; data PT_LOAD; meta PT_LOAD; }
SECTIONS
{
  . = 0;
  .text : {
    LONG(0x00000013)
    KEEP(*(.text.pex_main))
    *(.text .text.*)
    . = ALIGN(4);
  } :text
  .rodata : ALIGN(4) { *(.rodata .rodata.*) . = ALIGN(4); } :text
  .data : ALIGN(4) {
    __pex_rw_start = .;
    *(.pex.got*)
    *(.data .data.*)
    *(.sdata .sdata.*)
    . = ALIGN(4);
  } :data
  .bss (NOLOAD) : ALIGN(4) {
    *(.bss .bss.*)
    *(.sbss .sbss.*)
    *(COMMON)
    . = ALIGN(4);
    __pex_rw_end = .;
  } :data
INCLUDE pex_common_tail.ld
}
