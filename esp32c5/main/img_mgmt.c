/*
 * MCUmgr image group on the ESP32-C5.
 *
 * The file is the ESP-IDF app image with a 64-byte ECDSA P-256 signature
 * appended, the same trailer tools/ota_release.py writes. The signature is
 * not stored in the slot. The app bytes go to the inactive ota_N partition
 * through esp_ota_write. Nothing is marked bootable until state-write names
 * that file's hash. os reset then points otadata at it. The image stays
 * pending until state-write --confirm, and a reboot before that rolls back.
 */
#include "ota/smp.h"
#include "ota/ota_pubkey.h"
#include "crypto/sha256.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "nvs.h"
#include "psa/crypto.h"

#define SIG_LEN 64

enum {
    IMG_ERR_NO_IMAGE = 3,
    IMG_ERR_HASH_NOT_FOUND = 8,
    IMG_ERR_FLASH_WRITE_FAILED = 12,
    IMG_ERR_FLASH_CONTEXT_NOT_SET = 17,
    IMG_ERR_INVALID_LENGTH = 21,
    IMG_ERR_INVALID_IMAGE_HEADER_MAGIC = 23,
    IMG_ERR_INVALID_HASH = 24,
    IMG_ERR_CURRENT_VERSION_IS_NEWER = 27,
    IMG_ERR_IMAGE_ALREADY_PENDING = 28,
    IMG_ERR_INVALID_IMAGE_TOO_LARGE = 30,
    IMG_ERR_INVALID_IMAGE_DATA_OVERRUN = 31,
    IMG_ERR_IMAGE_CONFIRMATION_DENIED = 32,
    IMG_ERR_IMAGE_SETTING_TEST_TO_ACTIVE_DENIED = 33,
};

static const char *TAG = "ota";

typedef struct {
    uint8_t file[32];
    uint32_t img_len;
} slot_rec_t;

static struct {
    bool active;
    esp_ota_handle_t handle;
    const esp_partition_t *part;
    int target;
    uint32_t len;
    uint32_t off;
    uint8_t file_sha[32];
    bool have_file_sha;
    sha256_ctx_t img_sha;
    sha256_ctx_t file_ctx;
    uint8_t sig[SIG_LEN];
    bool saw_magic;
} up;

static bool test_armed;

static int part_slot(const esp_partition_t *p) {
    if (!p) return -1;
    if (p->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) return 0;
    if (p->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) return 1;
    return -1;
}

static const esp_partition_t *slot_part(int slot) {
    esp_partition_subtype_t sub = slot == 0 ? ESP_PARTITION_SUBTYPE_APP_OTA_0
                                            : ESP_PARTITION_SUBTYPE_APP_OTA_1;
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, sub, NULL);
}

static int running_slot(void) {
    return part_slot(esp_ota_get_running_partition());
}

static bool desc_of(int slot, esp_app_desc_t *out) {
    const esp_partition_t *p = slot_part(slot);
    if (!p) return false;
    return esp_ota_get_partition_description(p, out) == ESP_OK;
}

static int ver_cmp(const char *a, const char *b) {
    int ma = 0, mi = 0, mp = 0, ba = 0, bi = 0, bp = 0;
    if (sscanf(a, "%d.%d.%d", &ma, &mi, &mp) != 3) return 0;
    if (sscanf(b, "%d.%d.%d", &ba, &bi, &bp) != 3) return 0;
    if (ma != ba) return ma - ba;
    if (mi != bi) return mi - bi;
    return mp - bp;
}

static bool load_rec(int slot, slot_rec_t *r) {
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READONLY, &h) != ESP_OK) return false;
    char key[4] = { 's', (char)('0' + slot), 0, 0 };
    size_t n = sizeof *r;
    esp_err_t e = nvs_get_blob(h, key, r, &n);
    nvs_close(h);
    return e == ESP_OK && n == sizeof *r;
}

static bool save_rec(int slot, const slot_rec_t *r) {
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READWRITE, &h) != ESP_OK) return false;
    char key[4] = { 's', (char)('0' + slot), 0, 0 };
    esp_err_t e = nvs_set_blob(h, key, r, sizeof *r);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

static void drop_rec(int slot) {
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READWRITE, &h) != ESP_OK) return;
    char key[4] = { 's', (char)('0' + slot), 0, 0 };
    nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
}

static bool hash_names(int slot, const uint8_t *h) {
    slot_rec_t r;
    return load_rec(slot, &r) && memcmp(h, r.file, 32) == 0;
}

static bool sig_ok(const uint8_t digest[32], const uint8_t sig[64]) {
    /* Same trailer as the RP2350 image: SHA-256 of the app bytes, then a
     * raw P-256 r||s. ESP-IDF 6 does not export mbedtls/ecdsa.h. */
    uint8_t raw[65] = { 0x04 };
    memcpy(raw + 1, OTA_PUBKEY, 64);
    if (psa_crypto_init() != PSA_SUCCESS) return false;
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    mbedtls_svc_key_id_t key = 0;
    psa_status_t st = psa_import_key(&attr, raw, sizeof raw, &key);
    if (st != PSA_SUCCESS) return false;
    st = psa_verify_hash(key, PSA_ALG_ECDSA(PSA_ALG_SHA_256), digest, 32, sig, 64);
    psa_destroy_key(key);
    return st == PSA_SUCCESS;
}

static void abort_upload(void) {
    if (up.active) esp_ota_abort(up.handle);
    up.active = false;
}

static void version_str(int slot, char *dst, size_t cap) {
    esp_app_desc_t d;
    if (!desc_of(slot, &d) || d.version[0] == 0) snprintf(dst, cap, "0.0.0");
    else snprintf(dst, cap, "%s", d.version);
}

static bool pending_verify(void) {
    const esp_partition_t *me = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (!me || esp_ota_get_state_partition(me, &st) != ESP_OK) return false;
    return st == ESP_OTA_IMG_PENDING_VERIFY;
}

static void encode_state(cbor_enc_t *e) {
    int me = running_slot();
    cbor_tstr(e, "images");
    cbor_array_open(e);
    for (int i = 0; i < 2; i++) {
        char v[32];
        version_str(i, v, sizeof v);
        esp_app_desc_t d;
        bool bootable = desc_of(i, &d);
        slot_rec_t rec;
        bool have = load_rec(i, &rec);
        cbor_map_open(e);
        cbor_kv_uint(e, "image", 0);
        cbor_kv_uint(e, "slot", (uint64_t)i);
        cbor_kv_tstr(e, "version", v);
        if (have) cbor_kv_bstr(e, "hash", rec.file, 32);
        else { uint8_t z[32] = {0}; cbor_kv_bstr(e, "hash", z, 32); }
        cbor_kv_bool(e, "bootable", bootable);
        cbor_kv_bool(e, "pending", i != me && test_armed);
        cbor_kv_bool(e, "confirmed", i == me && !pending_verify());
        cbor_kv_bool(e, "active", i == me);
        cbor_kv_bool(e, "permanent", false);
        cbor_close(e);
    }
    cbor_close(e);
    cbor_kv_uint(e, "splitStatus", 0);
}

static int h_state_read(const smp_req_t *req, cbor_enc_t *e) {
    (void)req;
    if (running_slot() < 0) return MGMT_ERR_ENOTSUP;
    encode_state(e);
    return 0;
}

static int h_state_write(const smp_req_t *req, cbor_enc_t *e) {
    int me = running_slot();
    if (me < 0) return MGMT_ERR_ENOTSUP;
    bool confirm = false;
    cbor_get_bool(req->payload, req->plen, "confirm", &confirm);
    const uint8_t *hash = NULL;
    size_t hlen = 0;
    bool have_hash = cbor_get_bstr(req->payload, req->plen, "hash", &hash, &hlen) && hlen == 32;

    if (confirm) {
        if (have_hash && !hash_names(me, hash)) {
            if (hash_names(me ^ 1, hash)) return SMP_GERR(IMG_ERR_IMAGE_SETTING_TEST_TO_ACTIVE_DENIED);
            return SMP_GERR(IMG_ERR_HASH_NOT_FOUND);
        }
        if (pending_verify()) {
            if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK)
                return SMP_GERR(IMG_ERR_IMAGE_CONFIRMATION_DENIED);
            ESP_LOGI(TAG, "slot %c confirmed", "AB"[me]);
        }
    } else {
        if (!have_hash) return MGMT_ERR_EINVAL;
        if (!hash_names(me ^ 1, hash)) return SMP_GERR(IMG_ERR_HASH_NOT_FOUND);
        if (test_armed) return SMP_GERR(IMG_ERR_IMAGE_ALREADY_PENDING);
        char mine[32], other[32];
        version_str(me, mine, sizeof mine);
        version_str(me ^ 1, other, sizeof other);
        if (ver_cmp(other, mine) <= 0) return SMP_GERR(IMG_ERR_CURRENT_VERSION_IS_NEWER);
        test_armed = true;
        ESP_LOGI(TAG, "slot %c armed for test on the next reset", "AB"[me ^ 1]);
    }
    encode_state(e);
    return 0;
}

static int h_upload(const smp_req_t *req, cbor_enc_t *e) {
    int me = running_slot();
    if (me < 0) return MGMT_ERR_ENOTSUP;
    uint64_t off = 0, len = 0;
    if (!cbor_get_uint(req->payload, req->plen, "off", &off)) return MGMT_ERR_EINVAL;
    const uint8_t *data = NULL;
    size_t dlen = 0;
    cbor_get_bstr(req->payload, req->plen, "data", &data, &dlen);
    const uint8_t *sha = NULL;
    size_t slen = 0;
    bool have_sha = cbor_get_bstr(req->payload, req->plen, "sha", &sha, &slen) && slen == 32;
    const esp_partition_t *dest = slot_part(me ^ 1);
    if (!dest) return MGMT_ERR_ENOTSUP;

    if (off == 0) {
        if (!cbor_get_uint(req->payload, req->plen, "len", &len) || len <= SIG_LEN)
            return SMP_GERR(IMG_ERR_INVALID_LENGTH);
        if (len - SIG_LEN > dest->size) return SMP_GERR(IMG_ERR_INVALID_IMAGE_TOO_LARGE);
        if (up.active && have_sha && up.have_file_sha && memcmp(sha, up.file_sha, 32) == 0 && up.len == len) {
            cbor_kv_uint(e, "off", up.off);
            return 0;
        }
        abort_upload();
        memset(&up, 0, sizeof up);
        if (esp_ota_begin(dest, (size_t)(len - SIG_LEN), &up.handle) != ESP_OK)
            return SMP_GERR(IMG_ERR_FLASH_WRITE_FAILED);
        up.active = true;
        up.part = dest;
        up.target = me ^ 1;
        up.len = (uint32_t)len;
        if (have_sha) { memcpy(up.file_sha, sha, 32); up.have_file_sha = true; }
        sha256_init(&up.img_sha);
        sha256_init(&up.file_ctx);
        test_armed = false;
        drop_rec(up.target);
        ESP_LOGI(TAG, "upload of %u bytes into slot %c", (unsigned)len, "AB"[up.target]);
    } else if (!up.active) {
        return SMP_GERR(IMG_ERR_FLASH_CONTEXT_NOT_SET);
    } else if (off != up.off) {
        cbor_kv_uint(e, "off", up.off);
        return 0;
    }

    if (dlen && data) {
        if (up.off + dlen > up.len) { abort_upload(); return SMP_GERR(IMG_ERR_INVALID_IMAGE_DATA_OVERRUN); }
        uint32_t img_len = up.len - SIG_LEN;
        sha256_update(&up.file_ctx, data, dlen);
        size_t img_part = 0;
        if (up.off < img_len) img_part = (up.off + dlen <= img_len) ? dlen : img_len - up.off;
        if (img_part) {
            if (!up.saw_magic) {
                if (data[0] != ESP_IMAGE_HEADER_MAGIC) {
                    abort_upload();
                    return SMP_GERR(IMG_ERR_INVALID_IMAGE_HEADER_MAGIC);
                }
                up.saw_magic = true;
            }
            sha256_update(&up.img_sha, data, img_part);
            if (esp_ota_write(up.handle, data, img_part) != ESP_OK) {
                abort_upload();
                return SMP_GERR(IMG_ERR_FLASH_WRITE_FAILED);
            }
        }
        if (dlen > img_part) {
            size_t sig_off = up.off + img_part - img_len;
            memcpy(up.sig + sig_off, data + img_part, dlen - img_part);
        }
        up.off += (uint32_t)dlen;
    }

    if (up.off >= up.len) {
        uint8_t digest[32];
        sha256_final(&up.img_sha, digest);
        if (!sig_ok(digest, up.sig)) {
            abort_upload();
            ESP_LOGW(TAG, "signature rejected");
            return SMP_GERR(IMG_ERR_INVALID_HASH);
        }
        if (esp_ota_end(up.handle) != ESP_OK) {
            up.active = false;
            return SMP_GERR(IMG_ERR_FLASH_WRITE_FAILED);
        }
        up.active = false;
        slot_rec_t rec;
        sha256_final(&up.file_ctx, rec.file);
        rec.img_len = up.len - SIG_LEN;
        if (!save_rec(up.target, &rec)) return SMP_GERR(IMG_ERR_FLASH_WRITE_FAILED);
        char v[32];
        version_str(up.target, v, sizeof v);
        ESP_LOGI(TAG, "slot %c holds a verified image %s", "AB"[up.target], v);
    }
    cbor_kv_uint(e, "off", up.off);
    return 0;
}

static int h_erase(const smp_req_t *req, cbor_enc_t *e) {
    (void)req;
    (void)e;
    int me = running_slot();
    if (me < 0) return MGMT_ERR_ENOTSUP;
    abort_upload();
    test_armed = false;
    const esp_partition_t *p = slot_part(me ^ 1);
    drop_rec(me ^ 1);
    if (!p || esp_partition_erase_range(p, 0, 4096) != ESP_OK) return SMP_GERR(IMG_ERR_FLASH_WRITE_FAILED);
    return 0;
}

static int h_slot_info(const smp_req_t *req, cbor_enc_t *e) {
    (void)req;
    if (running_slot() < 0) return MGMT_ERR_ENOTSUP;
    cbor_tstr(e, "images");
    cbor_array_open(e);
    cbor_map_open(e);
    cbor_kv_uint(e, "image", 0);
    cbor_tstr(e, "slots");
    cbor_array_open(e);
    for (int i = 0; i < 2; i++) {
        const esp_partition_t *p = slot_part(i);
        cbor_map_open(e);
        cbor_kv_uint(e, "slot", (uint64_t)i);
        cbor_kv_uint(e, "size", p ? p->size : 0);
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

const esp_partition_t *img_mgmt_test_partition(void) {
    int me = running_slot();
    if (me < 0 || !test_armed) return NULL;
    return slot_part(me ^ 1);
}

const smp_cmd_t smp_img_cmds[] = {
    { SMP_GROUP_IMAGE, 0, false, h_state_read },
    { SMP_GROUP_IMAGE, 0, true,  h_state_write },
    { SMP_GROUP_IMAGE, 1, true,  h_upload },
    { SMP_GROUP_IMAGE, 5, true,  h_erase },
    { SMP_GROUP_IMAGE, 6, false, h_slot_info },
    { 0, 0, false, NULL },
};
