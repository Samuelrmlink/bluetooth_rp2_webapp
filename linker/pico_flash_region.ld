/* Overrides pico-sdk's own generated pico_flash_region.ld (see
 * src/rp2_common/pico_standard_link/pico_flash_region.template.ld), which
 * normally sizes this region from PICO_FLASH_SIZE_BYTES (the board's true
 * 4MiB on a Pico 2 W). We deliberately shrink *only* what the linker is
 * allowed to place code/rodata/data in, to one A/B slot (1024 KiB; see
 * partitions.json) — LittleFS owns 0x250000 on, then the PEX partition, and BTstack's NVM the last
 * 16KiB (see src/fs/fs.h's FS_FLASH_OFF/FS_SIZE/FS_PEX_OFF/FS_BT_TAIL and docs/flash-layout.md;
 * tools/run_tests.py checks the slot size here against partitions.json).
 *
 * PICO_FLASH_SIZE_BYTES itself is left untouched at the board's true
 * size (NOT redefined here, unlike pico_override_flash_size() — that
 * helper also redefines the PICO_FLASH_SIZE_BYTES compile macro, which
 * hardware_flash's flash_range_erase/flash_range_program hard-assert
 * against; shrinking it would make it impossible to use those functions
 * to write LittleFS data past this boundary). This file only touches the
 * linker's view of flash, so a firmware build that grows past 2MiB fails
 * to link ("region FLASH overflowed") instead of silently overlapping
 * LittleFS once flashed.
 */
FLASH(rx) : ORIGIN = 0x10000000, LENGTH = 1024 * 1024
