/* One SD driver behind the differential script (sddiff.c). Results are
 * classes, so two drivers with different error enums compare. */
#ifndef SDDIFF_BACKEND_H
#define SDDIFF_BACKEND_H
#include <stdbool.h>
#include <stdint.h>

enum { R_OK, R_TIMEOUT, R_CRC, R_IO, R_NOCARD, R_PARAM };

typedef struct {
    uint64_t sectors;
    uint8_t  cid[16];
    bool     cid_ok;
} be_info_t;

const char *be_name(void);
int  be_init(be_info_t *info);                               /* identify the card */
int  be_read(uint32_t lba, uint8_t *buf, uint32_t n);
int  be_write(uint32_t lba, const uint8_t *buf, uint32_t n);
int  be_sync(void);
bool be_present(void);

#endif
