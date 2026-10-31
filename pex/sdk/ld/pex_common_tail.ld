/* Shared tail of the PEX linker scripts: metadata pexlink reads and strips.
 * Placed far away from the image so nothing in it can be mistaken for image
 * bytes; pexlink refuses any other allocated section. */
  .pex.exports 0x70000000 : { KEEP(*(.pex.exports)) } :meta
  .pex.weak : { KEEP(*(.pex.weak)) } :meta
  .pex.meta : { KEEP(*(.pex.meta)) } :meta
  .pex.init : ALIGN(4) { KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*))) KEEP(*(.init_array)) } :meta
  .pex.fini : ALIGN(4) { KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*))) KEEP(*(.fini_array)) } :meta
  /DISCARD/ : {
    *(.ARM.exidx*) *(.ARM.extab*) *(.eh_frame*) *(.preinit_array*) *(.init) *(.fini)
    *(.comment) *(.note*) *(.gnu.attributes) *(.riscv.attributes)
  }
