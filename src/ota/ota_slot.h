/*
 * The two firmware slots, as the RP2350 boot ROM sees them.
 *
 * There is no bootloader of ours: the ROM reads the partition table at the
 * start of flash, picks the better of the A/B pair by its VERSION item,
 * sets up QMI address translation so the winner appears at 0x10000000, and
 * runs it. A try-before-you-buy image only boots via a FLASH_UPDATE reboot
 * and is reverted by the ROM's own ~16.7 s watchdog unless it is bought.
 *
 * Addresses here are STORAGE offsets (bytes from the start of flash), never
 * runtime ones: the running image cannot derive the other slot's address
 * from its own, because its own is translated.
 */
#ifndef OTA_SLOT_H
#define OTA_SLOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct { uint32_t start, size; } ota_part_t;   /* bytes, storage space */

void ota_slot_init(void);
int  ota_slot_active(void);                  /* 0 or 1; -1 = not booted from a slot */
bool ota_slot_get(int which, ota_part_t *out);
/* Reads of a slot's bytes go through the NOTRANSLATE alias, which is what
 * the SDK's own partition reader uses: the translated window has a hole
 * past the running image. */
const uint8_t *ota_slot_bytes(int which);

uint8_t ota_boot_type(void);                 /* BOOT_TYPE_* from the ROM */
bool ota_buy_pending(void);                  /* TBYB image not yet bought */
bool ota_bought(void);                       /* this boot bought itself */
void ota_reserve_buy_buf(void);              /* hold the 4 KB ROM work area */
int  ota_buy(void);                          /* rom_explicit_buy, once */

/* The VERSION item of the picobin block at the start of a slot. */
bool ota_slot_version(int which, uint16_t *major, uint16_t *minor);
void ota_slot_version_str(int which, char *dst, size_t cap);

/* 4 KB sector erase / 256 B page program via rom_flash_op (storage space,
 * permission-checked against the partition table). Return 0 or a ROM error. */
int ota_slot_erase_sector(int which, uint32_t off);
int ota_slot_program_page(int which, uint32_t off, const uint8_t page[256]);

/* Reboot: into the other slot with a flash-update boot (the TBYB path), or
 * a plain reboot. Never returns. */
void ota_reboot(bool flash_update_other);

#endif
