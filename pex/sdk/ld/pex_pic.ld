/* PEX code model B (ROPI + RWPI), spec 6.2: text+rodata from address 0 (one
 * segment, valid anywhere with no patching), data+bss+GOT as a separate
 * segment whose start is the static base r9 points at. Its link address is
 * arbitrary (0x40000000) -- the code never contains it. */
ENTRY(pex_main)
PHDRS { text PT_LOAD; data PT_LOAD; meta PT_LOAD; }
SECTIONS
{
  . = 0;
  .text : { KEEP(*(.text.pex_main)) *(.text .text.*) . = ALIGN(4); } :text
  .rodata : ALIGN(4) { *(.rodata .rodata.*) . = ALIGN(4); } :text
  .data 0x40000000 : { __pex_rw_start = .; *(.pex.got*) *(.data .data.*) . = ALIGN(4); } :data
  .bss (NOLOAD) : ALIGN(4) { *(.bss .bss.*) *(COMMON) . = ALIGN(4); __pex_rw_end = .; } :data
INCLUDE pex_common_tail.ld
}
