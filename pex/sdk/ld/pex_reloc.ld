/* PEX code model A (relocated at load), spec 6.1: text, rodata and data
 * contiguous from address 0, bss after them. Link with --emit-relocs
 * --gc-sections; pexlink turns the absolute words into the RELOC lists. */
ENTRY(pex_main)
PHDRS { text PT_LOAD; data PT_LOAD; meta PT_LOAD; }
SECTIONS
{
  . = 0;
  .text : { KEEP(*(.text.pex_main)) *(.text .text.*) *(.glue_7) *(.glue_7t) *(.vfp11_veneer) *(.v4_bx) . = ALIGN(4); } :text
  .rodata : ALIGN(4) { *(.rodata .rodata.*) . = ALIGN(4); } :text
  .data : ALIGN(8) { __pex_rw_start = .; *(.pex.got*) *(.data .data.*) . = ALIGN(4); } :data
  .bss (NOLOAD) : ALIGN(4) { *(.bss .bss.*) *(COMMON) . = ALIGN(4); __pex_rw_end = .; } :data
INCLUDE pex_common_tail.ld
}
