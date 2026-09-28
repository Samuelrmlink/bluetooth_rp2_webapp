/*
 * MCUmgr image group (1): list, upload, test/confirm, erase.
 *
 * The release file is `btgw.bin || sig` where sig is 64 raw bytes of ECDSA
 * P-256 (r||s) over SHA-256(btgw.bin) -- see tools/ota_release.py. SMP's
 * upload has no signature field, so it rides on the end; `len` covers both.
 * The device hashes the image part as it lands and checks the signature at
 * the end of the upload with the public key baked into this firmware
 * (ota_pubkey.h). An image that fails is left with its first sector erased,
 * so the ROM will never consider it, and the next upload overwrites it.
 *
 * Flash is written HERE, from the main loop (smp_udp.c guarantees that):
 * one sector erase (~45 ms) at most per chunk, then pages. The client is
 * lock-step so this also throttles it to one chunk per completed write.
 */
#include "ota/smp.h"
#include "ota/ota_slot.h"
#include "ota/ota_pubkey.h"
#include "crypto/sha256.h"
#include "uECC.h"

/* The ROM clears the try-before-you-buy flag in the picobin block in the
 * FIRST sector when an image is bought, so those bytes are not stable after
 * a boot. Integrity re-checks hash the image EXCLUDING this sector. */
#define IMG_SECTOR0 4096u

#include <stdio.h>
#include <string.h>
#include "pico/time.h"
#include "fs/fs.h"

/* Group-specific codes (zephyr img_mgmt.h), the ones we raise. */
enum {
    IMG_ERR_FLASH_CONFIG_QUERY_FAIL = 2, IMG_ERR_NO_IMAGE = 3, IMG_ERR_NO_TLVS = 4,
    IMG_ERR_INVALID_TLV = 5, IMG_ERR_TLV_MULTIPLE_HASHES_FOUND = 6, IMG_ERR_TLV_INVALID_SIZE = 7,
    IMG_ERR_HASH_NOT_FOUND = 8, IMG_ERR_NO_FREE_SLOT = 9, IMG_ERR_FLASH_OPEN_FAILED = 10,
    IMG_ERR_FLASH_READ_FAILED = 11, IMG_ERR_FLASH_WRITE_FAILED = 12, IMG_ERR_FLASH_ERASE_FAILED = 13,
    IMG_ERR_INVALID_SLOT = 14, IMG_ERR_NO_FREE_MEMORY = 15, IMG_ERR_FLASH_CONTEXT_ALREADY_SET = 16,
    IMG_ERR_FLASH_CONTEXT_NOT_SET = 17, IMG_ERR_FLASH_AREA_DEVICE_NULL = 18,
    IMG_ERR_INVALID_PAGE_OFFSET = 19, IMG_ERR_INVALID_OFFSET = 20, IMG_ERR_INVALID_LENGTH = 21,
    IMG_ERR_INVALID_IMAGE_HEADER = 22, IMG_ERR_INVALID_IMAGE_HEADER_MAGIC = 23,
    IMG_ERR_INVALID_HASH = 24, IMG_ERR_INVALID_FLASH_ADDRESS = 25, IMG_ERR_VERSION_GET_FAILED = 26,
    IMG_ERR_CURRENT_VERSION_IS_NEWER = 27, IMG_ERR_IMAGE_ALREADY_PENDING = 28,
    IMG_ERR_INVALID_IMAGE_VECTOR_TABLE = 29, IMG_ERR_INVALID_IMAGE_TOO_LARGE = 30,
    IMG_ERR_INVALID_IMAGE_DATA_OVERRUN = 31, IMG_ERR_IMAGE_CONFIRMATION_DENIED = 32,
    IMG_ERR_IMAGE_SETTING_TEST_TO_ACTIVE_DENIED = 33,
};

#define SIG_LEN 64

static struct {
    bool active;
    int target;               /* slot index being written */
    uint32_t len;             /* whole file, image + signature */
    uint32_t off;             /* bytes accepted so far */
    uint8_t file_sha[32];     /* client's identity for the upload (resume) */
    bool have_file_sha;
    sha256_ctx_t img_sha;     /* over the image part only: what the signature covers */
    sha256_ctx_t file_ctx;    /* over the whole file: what a client computes for state-write */
    sha256_ctx_t body_ctx;    /* over image bytes past sector 0: stable across a TBYB buy */
    uint8_t sig[SIG_LEN];
    uint32_t erased;          /* sectors erased so far (next erase at erased*4096) */
    uint8_t page[256];
    uint32_t page_fill;
    /* The first sector holds the picobin block (version, hash, TBYB flag):
     * the moment it is in flash the ROM -- and our own image list -- see a
     * bootable image. So it is held here and programmed LAST, after the
     * signature has verified; a partial or rejected upload leaves the slot
     * with an erased first sector and nothing that could ever be picked. */
    uint8_t sec0[4096];
    uint32_t page_off;        /* slot offset the page buffer starts at */
    uint32_t started_ms;
} up;

static bool test_armed;      /* state write {confirm:false} on the other slot */
/* Per slot, in LittleFS (ota/slotN.rec): file hash (32) | image digest (32) |
 * image length (4). The file hash is what `image list` reports and what a
 * client will send back in state-write -- smpmgr sends no `sha` with its
 * upload and hashes the file it has; the image digest is what the signature
 * covers and lets ota.status re-check the slot's bytes at any time. */
typedef struct { uint8_t file[32], img[32], body[32]; uint32_t img_len; } slot_rec_t;
static slot_rec_t slot_rec[2];
static bool slot_hash_ok[2];

static void rec_path(int which, char *dst, size_t cap) { snprintf(dst, cap, "ota/slot%d.rec", which); }

static void load_hashes(void) {
    for (int i = 0; i < 2; i++) {
        char p[24]; rec_path(i, p, sizeof p);
        size_t size;
        int h = fs_begin_read(p, &size);
        if (h < 0) { slot_hash_ok[i] = false; continue; }
        slot_hash_ok[i] = size == sizeof(slot_rec_t) &&
                          fs_read(h, (uint8_t *)&slot_rec[i], sizeof(slot_rec_t)) == (int)sizeof(slot_rec_t);
        fs_end_read(h);
    }
}

static void save_rec(int which, const slot_rec_t *r) {
    char p[24]; rec_path(which, p, sizeof p);
    if (fs_begin_write(p) == 0) {
        fs_write((const uint8_t *)r, sizeof *r);
        fs_end_write();
    }
    slot_rec[which] = *r;
    slot_hash_ok[which] = true;
}

/* Does a hash a client sent name this slot? Either the file hash or the
 * image digest counts: a client that read `image list` has the former, one
 * that hashed our release file has it too, one that hashed btgw.bin alone
 * has the latter. */
static bool hash_names_slot(int which, const uint8_t *h) {
    return slot_hash_ok[which] &&
           (memcmp(h, slot_rec[which].file, 32) == 0 || memcmp(h, slot_rec[which].img, 32) == 0);
}

static int flush_page(void) {
    if (up.page_fill == 0) return 0;
    memset(up.page + up.page_fill, 0xff, 256 - up.page_fill);
    int rc = 0;
    if (up.page_off < sizeof up.sec0) memcpy(up.sec0 + up.page_off, up.page, 256);   /* held back */
    else rc = ota_slot_program_page(up.target, up.page_off, up.page);
    up.page_fill = 0;
    up.page_off += 256;
    return rc;
}

static void abort_upload(void) {
    up.active = false;
}

static int write_bytes(const uint8_t *d, size_t n) {
    while (n) {
        /* Erase lazily as the write front crosses each sector. */
        uint32_t need = (up.page_off + up.page_fill) / 4096 + 1;
        while (up.erased < need) {
            if (ota_slot_erase_sector(up.target, up.erased * 4096) != 0) return SMP_GERR(IMG_ERR_FLASH_ERASE_FAILED);
            up.erased++;
        }
        size_t take = 256 - up.page_fill;
        if (take > n) take = n;
        memcpy(up.page + up.page_fill, d, take);
        up.page_fill += take; d += take; n -= take;
        if (up.page_fill == 256 && flush_page() != 0) return SMP_GERR(IMG_ERR_FLASH_WRITE_FAILED);
    }
    return 0;
}

static void encode_state(cbor_enc_t *e) {
    static bool loaded;
    if (!loaded) { load_hashes(); loaded = true; }
    int me = ota_slot_active();
    cbor_tstr(e, "images");
    cbor_array_open(e);
    for (int i = 0; i < 2 && me >= 0; i++) {
        char v[16];
        ota_slot_version_str(i, v, sizeof v);
        uint16_t ma, mi;
        bool has_image = ota_slot_version(i, &ma, &mi);
        cbor_map_open(e);
        cbor_kv_uint(e, "image", 0);
        cbor_kv_uint(e, "slot", (uint64_t)i);
        cbor_kv_tstr(e, "version", v);
        if (slot_hash_ok[i]) cbor_kv_bstr(e, "hash", slot_rec[i].file, 32);
        else { uint8_t z[32] = {0}; cbor_kv_bstr(e, "hash", z, 32); }
        cbor_kv_bool(e, "bootable", has_image);
        cbor_kv_bool(e, "pending", i != me && test_armed);
        cbor_kv_bool(e, "confirmed", i == me && !ota_buy_pending());
        cbor_kv_bool(e, "active", i == me);
        cbor_kv_bool(e, "permanent", false);
        cbor_close(e);
    }
    cbor_close(e);
    cbor_kv_uint(e, "splitStatus", 0);
}

static int h_state_read(const smp_req_t *req, cbor_enc_t *e) {
    (void)req;
    if (ota_slot_active() < 0) return MGMT_ERR_ENOTSUP;
    encode_state(e);
    return 0;
}

static int h_state_write(const smp_req_t *req, cbor_enc_t *e) {
    int me = ota_slot_active();
    if (me < 0) return MGMT_ERR_ENOTSUP;
    bool confirm = false;
    cbor_get_bool(req->payload, req->plen, "confirm", &confirm);
    const uint8_t *hash; size_t hlen;
    bool have_hash = cbor_get_bstr(req->payload, req->plen, "hash", &hash, &hlen) && hlen == 32;

    if (confirm) {
        /* Confirm THIS image (no hash, or our hash): buy if pending. A hash
         * naming the other slot cannot be confirmed without booting it. */
        if (have_hash && !hash_names_slot(me, hash)) {
            if (hash_names_slot(me ^ 1, hash))
                return SMP_GERR(IMG_ERR_IMAGE_SETTING_TEST_TO_ACTIVE_DENIED);
            return SMP_GERR(IMG_ERR_HASH_NOT_FOUND);
        }
        if (ota_buy_pending() && ota_buy() != 0) return SMP_GERR(IMG_ERR_IMAGE_CONFIRMATION_DENIED);
    } else {
        /* Test: arm a flash-update boot into the other slot on the next reset. */
        if (!have_hash) return MGMT_ERR_EINVAL;
        if (!hash_names_slot(me ^ 1, hash)) return SMP_GERR(IMG_ERR_HASH_NOT_FOUND);
        uint16_t ma, mi;
        if (!ota_slot_version(me ^ 1, &ma, &mi)) return SMP_GERR(IMG_ERR_NO_IMAGE);
        test_armed = true;
        printf("ota: slot %c armed for test on the next reset\n", "AB"[me ^ 1]);
    }
    encode_state(e);
    return 0;
}

static int h_upload(const smp_req_t *req, cbor_enc_t *e) {
    int me = ota_slot_active();
    if (me < 0) return MGMT_ERR_ENOTSUP;
    uint64_t off = 0, len = 0, image = 0;
    if (!cbor_get_uint(req->payload, req->plen, "off", &off)) return MGMT_ERR_EINVAL;
    cbor_get_uint(req->payload, req->plen, "image", &image);
    const uint8_t *data = NULL; size_t dlen = 0;
    cbor_get_bstr(req->payload, req->plen, "data", &data, &dlen);
    const uint8_t *sha = NULL; size_t slen = 0;
    bool have_sha = cbor_get_bstr(req->payload, req->plen, "sha", &sha, &slen) && slen == 32;
    ota_part_t part;
    ota_slot_get(me ^ 1, &part);

    if (off == 0) {
        if (!cbor_get_uint(req->payload, req->plen, "len", &len) || len <= SIG_LEN) return SMP_GERR(IMG_ERR_INVALID_LENGTH);
        if (len > part.size) return SMP_GERR(IMG_ERR_INVALID_IMAGE_TOO_LARGE);
        /* Resume: the same file (by sha) already in progress -> report where
         * we are and let the client skip ahead. */
        if (up.active && have_sha && up.have_file_sha && memcmp(sha, up.file_sha, 32) == 0 && up.len == len) {
            cbor_kv_uint(e, "off", up.off);
            return 0;
        }
        memset(&up, 0, sizeof up);
        up.active = true;
        up.target = me ^ 1;
        up.len = (uint32_t)len;
        if (have_sha) { memcpy(up.file_sha, sha, 32); up.have_file_sha = true; }
        sha256_init(&up.img_sha);
        sha256_init(&up.file_ctx);
        sha256_init(&up.body_ctx);
        memset(up.sec0, 0xff, sizeof up.sec0);
        up.started_ms = to_ms_since_boot(get_absolute_time());
        test_armed = false;
        slot_hash_ok[up.target] = false;
        printf("ota: upload of %lu bytes into slot %c begins\n", (unsigned long)len, "AB"[up.target]);
    } else if (!up.active) {
        return SMP_GERR(IMG_ERR_FLASH_CONTEXT_NOT_SET);
    } else if (off != up.off) {
        /* Out of step: tell the client where we are (this is the resume
         * protocol -- not an error). */
        cbor_kv_uint(e, "off", up.off);
        return 0;
    }
    if (dlen && data) {
        if (up.off + dlen > up.len) { abort_upload(); return SMP_GERR(IMG_ERR_INVALID_IMAGE_DATA_OVERRUN); }
        uint32_t img_len = up.len - SIG_LEN;
        sha256_update(&up.file_ctx, data, dlen);
        /* Split the chunk at the image/signature boundary. */
        size_t img_part = 0;
        if (up.off < img_len) img_part = (up.off + dlen <= img_len) ? dlen : img_len - up.off;
        if (img_part) {
            sha256_update(&up.img_sha, data, img_part);
            /* body = the image minus its first sector (see IMG_SECTOR0). */
            if (up.off + img_part > IMG_SECTOR0) {
                size_t skip = up.off >= IMG_SECTOR0 ? 0 : (size_t)(IMG_SECTOR0 - up.off);
                sha256_update(&up.body_ctx, data + skip, img_part - skip);
            }
            int rc = write_bytes(data, img_part);
            if (rc) { abort_upload(); return rc; }
        }
        if (dlen > img_part) {
            size_t sig_off = up.off + img_part - img_len;
            memcpy(up.sig + sig_off, data + img_part, dlen - img_part);
        }
        up.off += dlen;
        if ((up.off >> 17) != ((up.off - dlen) >> 17))          /* every 128 KB */
            printf("ota: %lu/%lu bytes\n", (unsigned long)up.off, (unsigned long)up.len);
    }
    if (up.off >= up.len) {
        int rc = flush_page();
        if (rc) { abort_upload(); return SMP_GERR(IMG_ERR_FLASH_WRITE_FAILED); }
        uint8_t digest[32];
        sha256_final(&up.img_sha, digest);
        bool ok = uECC_verify(OTA_PUBKEY, digest, up.sig) == 1;
        if (!ok) {
            /* Leave nothing bootable behind. */
            ota_slot_erase_sector(up.target, 0);
            abort_upload();
            printf("ota: signature REJECTED; slot %c invalidated\n", "AB"[up.target]);
            return SMP_GERR(IMG_ERR_INVALID_HASH);
        }
        /* Verified: now the first sector, which makes the slot bootable. */
        for (uint32_t o = 0; o < sizeof up.sec0; o += 256) {
            if (ota_slot_program_page(up.target, o, up.sec0 + o) != 0) {
                ota_slot_erase_sector(up.target, 0);
                abort_upload();
                return SMP_GERR(IMG_ERR_FLASH_WRITE_FAILED);
            }
        }
        slot_rec_t r;
        sha256_final(&up.file_ctx, r.file);
        sha256_final(&up.body_ctx, r.body);
        memcpy(r.img, digest, 32);
        r.img_len = up.len - SIG_LEN;
        save_rec(up.target, &r);
        char v[16]; ota_slot_version_str(up.target, v, sizeof v);
        printf("ota: slot %c holds a verified image v%s (%lu bytes in %lu ms)\n", "AB"[up.target], v,
               (unsigned long)up.len, (unsigned long)(to_ms_since_boot(get_absolute_time()) - up.started_ms));
        up.active = false;
    }
    cbor_kv_uint(e, "off", up.off);
    return 0;
}

static int h_erase(const smp_req_t *req, cbor_enc_t *e) {
    (void)req; (void)e;
    int me = ota_slot_active();
    if (me < 0) return MGMT_ERR_ENOTSUP;
    abort_upload();
    test_armed = false;
    slot_hash_ok[me ^ 1] = false;
    if (ota_slot_erase_sector(me ^ 1, 0) != 0) return SMP_GERR(IMG_ERR_FLASH_ERASE_FAILED);
    return 0;
}

static int h_slot_info(const smp_req_t *req, cbor_enc_t *e) {
    (void)req;
    int me = ota_slot_active();
    if (me < 0) return MGMT_ERR_ENOTSUP;
    cbor_tstr(e, "images");
    cbor_array_open(e);
    cbor_map_open(e);
    cbor_kv_uint(e, "image", 0);
    cbor_tstr(e, "slots");
    cbor_array_open(e);
    for (int i = 0; i < 2; i++) {
        ota_part_t p; ota_slot_get(i, &p);
        cbor_map_open(e);
        cbor_kv_uint(e, "slot", (uint64_t)i);
        cbor_kv_uint(e, "size", p.size);
        cbor_kv_uint(e, "upload_image_id", 0);
        cbor_close(e);
    }
    cbor_close(e);
    cbor_kv_uint(e, "max_image_size", 0);
    cbor_close(e);
    cbor_close(e);
    return 0;
}

bool img_mgmt_test_armed(void) { return test_armed; }
bool img_mgmt_upload_active(uint32_t *off, uint32_t *len) {
    if (off) *off = up.off;
    if (len) *len = up.len;
    return up.active;
}
/* Re-hash the slot's bytes and compare with the digest recorded at upload:
 * 0 = matches, 1 = differs, -1 = no record. ~30 ms for 660 KB. */
int img_mgmt_slot_check(int which) {
    static bool loaded;
    if (!loaded) { load_hashes(); loaded = true; }
    if (!slot_hash_ok[which]) return -1;
    const uint8_t *b = ota_slot_bytes(which);
    ota_part_t p;
    if (!b || !ota_slot_get(which, &p) || slot_rec[which].img_len > p.size) return -1;
    /* Hash the body (past sector 0), which the ROM does not touch when it
     * buys an image -- so this stays 0 after a boot, unlike a full-image
     * hash. Sector 0's own integrity is the ROM's picobin hash at boot. */
    uint32_t img_len = slot_rec[which].img_len;
    uint32_t body_len = img_len > IMG_SECTOR0 ? img_len - IMG_SECTOR0 : 0;
    uint8_t d[32];
    sha256(b + IMG_SECTOR0, body_len, d);
    return memcmp(d, slot_rec[which].body, 32) == 0 ? 0 : 1;
}

/* The file hash recorded for a slot (SHA-256 of image||signature), for
 * ota.status -- the value a client passes to state-write. */
bool img_mgmt_slot_file_hash(int which, uint8_t out[32]) {
    static bool loaded;
    if (!loaded) { load_hashes(); loaded = true; }
    if (which < 0 || which > 1 || !slot_hash_ok[which]) return false;
    memcpy(out, slot_rec[which].file, 32);
    return true;
}

const smp_cmd_t smp_img_cmds[] = {
    { SMP_GROUP_IMAGE, 0, false, h_state_read },
    { SMP_GROUP_IMAGE, 0, true,  h_state_write },
    { SMP_GROUP_IMAGE, 1, true,  h_upload },
    { SMP_GROUP_IMAGE, 5, true,  h_erase },
    { SMP_GROUP_IMAGE, 6, false, h_slot_info },
    { 0, 0, false, NULL },
};
