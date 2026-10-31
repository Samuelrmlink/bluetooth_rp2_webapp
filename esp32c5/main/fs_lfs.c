#include "fs_lfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lfs.h"

static const char *TAG = "fs";
static const esp_partition_t *part;
static lfs_t lfs;
static struct lfs_config cfg;
static uint8_t read_buf[256], prog_buf[256], lookahead[32];
static SemaphoreHandle_t mu;
static bool mounted;

static int bd_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *buf, lfs_size_t size) {
    (void)c;
    esp_err_t e = esp_partition_read(part, block * 4096u + off, buf, size);
    return e == ESP_OK ? 0 : LFS_ERR_IO;
}
static int bd_prog(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, const void *buf, lfs_size_t size) {
    (void)c;
    esp_err_t e = esp_partition_write(part, block * 4096u + off, buf, size);
    return e == ESP_OK ? 0 : LFS_ERR_IO;
}
static int bd_erase(const struct lfs_config *c, lfs_block_t block) {
    (void)c;
    esp_err_t e = esp_partition_erase_range(part, block * 4096u, 4096);
    return e == ESP_OK ? 0 : LFS_ERR_IO;
}
static int bd_sync(const struct lfs_config *c) { (void)c; return 0; }

static bool path_ok(const char *p) {
    if (!p || !p[0] || p[0] == '/' || strlen(p) > 128) return false;
    if (strstr(p, "..")) return false;
    for (const char *s = p; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c == '\\') return false;
    }
    return true;
}

void fs_lock(void) { xSemaphoreTakeRecursive(mu, portMAX_DELAY); }
void fs_unlock(void) { xSemaphoreGiveRecursive(mu); }

int fs_mount(void) {
    part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "littlefs");
    if (!part || (part->size % 4096) != 0) {
        ESP_LOGE(TAG, "no littlefs partition");
        return -1;
    }
    mu = xSemaphoreCreateRecursiveMutex();
    memset(&cfg, 0, sizeof cfg);
    cfg.read = bd_read;
    cfg.prog = bd_prog;
    cfg.erase = bd_erase;
    cfg.sync = bd_sync;
    cfg.read_size = 256;
    cfg.prog_size = 256;
    cfg.block_size = 4096;
    cfg.block_count = part->size / 4096;
    cfg.cache_size = 256;
    cfg.lookahead_size = sizeof lookahead;
    cfg.block_cycles = 500;
    cfg.read_buffer = read_buf;
    cfg.prog_buffer = prog_buf;
    cfg.lookahead_buffer = lookahead;
    int err = lfs_mount(&lfs, &cfg);
    if (err) {
        ESP_LOGW(TAG, "mount failed (%d); formatting %u blocks", err, (unsigned)cfg.block_count);
        err = lfs_format(&lfs, &cfg);
        if (!err) err = lfs_mount(&lfs, &cfg);
    }
    if (err) {
        ESP_LOGE(TAG, "littlefs unavailable (%d)", err);
        return -1;
    }
    mounted = true;
    ESP_LOGI(TAG, "mounted %u bytes", (unsigned)part->size);
    return 0;
}

bool fs_stat(const char *path, size_t *size) {
    if (!mounted || !path_ok(path)) return false;
    struct lfs_info info;
    fs_lock();
    int err = lfs_stat(&lfs, path, &info);
    fs_unlock();
    if (err < 0 || info.type != LFS_TYPE_REG) return false;
    if (size) *size = info.size;
    return true;
}

bool fs_is_dir(const char *path) {
    if (!mounted) return false;
    if (!path || !path[0] || strcmp(path, "/") == 0) return true;
    if (!path_ok(path)) return false;
    struct lfs_info info;
    fs_lock();
    int err = lfs_stat(&lfs, path, &info);
    fs_unlock();
    return err >= 0 && info.type == LFS_TYPE_DIR;
}

int fs_read_file(const char *path, void *dst, size_t cap, size_t *got) {
    if (!mounted || !path_ok(path) || !dst) return -1;
    lfs_file_t f;
    fs_lock();
    int err = lfs_file_open(&lfs, &f, path, LFS_O_RDONLY);
    lfs_ssize_t n = err < 0 ? err : lfs_file_read(&lfs, &f, dst, cap);
    if (err >= 0) lfs_file_close(&lfs, &f);
    fs_unlock();
    if (n < 0) return -1;
    if (got) *got = (size_t)n;
    return 0;
}

static int mkdir_one(const char *path) {
    int err = lfs_mkdir(&lfs, path);
    if (err == LFS_ERR_EXIST) return 0;
    return err;
}

int fs_mkdir_p(const char *path) {
    if (!mounted || !path_ok(path)) return -1;
    char tmp[140];
    snprintf(tmp, sizeof tmp, "%s", path);
    fs_lock();
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        int err = mkdir_one(tmp);
        *p = '/';
        if (err) { fs_unlock(); return -1; }
    }
    int err = mkdir_one(tmp);
    fs_unlock();
    return err ? -1 : 0;
}

int fs_write_file(const char *path, const void *src, size_t n) {
    if (!mounted || !path_ok(path)) return -1;
    char parent[140];
    snprintf(parent, sizeof parent, "%s", path);
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) {
        *slash = 0;
        if (fs_mkdir_p(parent)) return -1;
    }
    lfs_file_t f;
    fs_lock();
    int err = lfs_file_open(&lfs, &f, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (!err && n) {
        lfs_ssize_t w = lfs_file_write(&lfs, &f, src, n);
        if (w != (lfs_ssize_t)n) err = -1;
    }
    if (lfs_file_close(&lfs, &f) < 0) err = -1;
    fs_unlock();
    return err ? -1 : 0;
}

int fs_delete(const char *path) {
    if (!mounted || !path_ok(path)) return -1;
    fs_lock();
    int err = lfs_remove(&lfs, path);
    fs_unlock();
    return err ? -1 : 0;
}

int fs_rename(const char *from, const char *to) {
    if (!mounted || !path_ok(from) || !path_ok(to)) return -1;
    struct lfs_info info;
    fs_lock();
    if (lfs_stat(&lfs, to, &info) >= 0) { fs_unlock(); return 1; }
    int err = lfs_rename(&lfs, from, to);
    fs_unlock();
    return err ? -1 : 0;
}

static uint32_t ctz_blocks(uint32_t size) {
    if (!size) return 0;
    uint32_t off = size - 1;
    uint32_t b = 4096 - 8;
    uint32_t i = off / b;
    if (i == 0) return 1;
    i = (off - 4 * ((uint32_t)__builtin_popcount(i - 1) + 2)) / b;
    return i + 1;
}

int fs_list_json(const char *dir, char *buf, size_t cap) {
    if (!mounted || !buf || cap < 32) return -1;
    const char *open_path = "/";
    const char *shown = "";
    if (dir && dir[0] && strcmp(dir, "/") != 0) {
        if (!fs_is_dir(dir)) return -1;
        open_path = dir;
        shown = dir;
    }
    size_t n = 0;
    if (shown[0]) n += (size_t)snprintf(buf + n, cap - n, "{\"dir\":\"%s\",\"block\":4096,\"files\":[", shown);
    else n += (size_t)snprintf(buf + n, cap - n, "{\"block\":4096,\"files\":[");
    lfs_dir_t d;
    fs_lock();
    if (lfs_dir_open(&lfs, &d, open_path) < 0) {
        fs_unlock();
        if (shown[0]) snprintf(buf, cap, "{\"dir\":\"%s\",\"files\":[]}", shown);
        else snprintf(buf, cap, "{\"files\":[]}");
        return 0;
    }
    struct lfs_info info;
    int first = 1;
    while (lfs_dir_read(&lfs, &d, &info) > 0) {
        if (!strcmp(info.name, ".") || !strcmp(info.name, "..")) continue;
        if (n + 96 >= cap) break;
        if (!first) n += (size_t)snprintf(buf + n, cap - n, ",");
        first = 0;
        if (info.type == LFS_TYPE_DIR) {
            n += (size_t)snprintf(buf + n, cap - n, "{\"name\":\"%s\",\"dir\":true}", info.name);
        } else {
            unsigned alloc = info.size ? ctz_blocks(info.size) * 4096u : 0;
            n += (size_t)snprintf(buf + n, cap - n, "{\"name\":\"%s\",\"size\":%u,\"alloc\":%u}",
                                  info.name, (unsigned)info.size, alloc);
        }
    }
    lfs_dir_close(&lfs, &d);
    fs_unlock();
    snprintf(buf + n, cap - n, "]}");
    return 0;
}

struct fs_rd { lfs_file_t f; };
struct fs_wr { lfs_file_t f; bool open; };

fs_rd_t *fs_open_read(const char *path, uint32_t *size) {
    if (!mounted || !path_ok(path)) return NULL;
    fs_rd_t *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    fs_lock();
    int err = lfs_file_open(&lfs, &r->f, path, LFS_O_RDONLY);
    if (!err && size) *size = (uint32_t)lfs_file_size(&lfs, &r->f);
    fs_unlock();
    if (err) { free(r); return NULL; }
    return r;
}

int fs_pread(fs_rd_t *f, uint32_t off, void *dst, uint32_t n) {
    if (!f || !n) return n ? -1 : 0;
    fs_lock();
    lfs_soff_t s = lfs_file_seek(&lfs, &f->f, (lfs_soff_t)off, LFS_SEEK_SET);
    lfs_ssize_t r = s < 0 ? s : lfs_file_read(&lfs, &f->f, dst, n);
    fs_unlock();
    return r == (lfs_ssize_t)n ? (int)n : -1;
}

void fs_close_read(fs_rd_t *f) {
    if (!f) return;
    fs_lock();
    lfs_file_close(&lfs, &f->f);
    fs_unlock();
    free(f);
}

fs_wr_t *fs_open_write(const char *path) {
    if (!mounted || !path_ok(path)) return NULL;
    char parent[140];
    snprintf(parent, sizeof parent, "%s", path);
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) { *slash = 0; if (fs_mkdir_p(parent)) return NULL; }
    fs_wr_t *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    fs_lock();
    int err = lfs_file_open(&lfs, &w->f, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    fs_unlock();
    if (err) { free(w); return NULL; }
    w->open = true;
    return w;
}

int fs_pwrite(fs_wr_t *f, const void *src, uint32_t n) {
    if (!f || !f->open) return -1;
    if (!n) return 0;
    fs_lock();
    lfs_ssize_t r = lfs_file_write(&lfs, &f->f, src, n);
    fs_unlock();
    return r == (lfs_ssize_t)n ? 0 : -1;
}

int fs_close_write(fs_wr_t *f, bool abort_write) {
    if (!f) return -1;
    fs_lock();
    int err = 0;
    if (f->open) {
        if (abort_write) lfs_file_close(&lfs, &f->f);
        else err = lfs_file_close(&lfs, &f->f);
        f->open = false;
    }
    fs_unlock();
    free(f);
    return err < 0 ? -1 : 0;
}
