#include "lua/lfile_path.h"

#include <string.h>

static bool lfs_char(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '.' || c == '_' || c == '-';
}

static bool sd_char(unsigned char c) {
    if (c < 0x20 || c == 0x7F) { return false; }
    return !strchr("\\:*?\"<>|", (int)c);
}

int lfile_parse_path(const char *in, size_t len, char *out, size_t cap) {
    if (!in || !out || cap < 1) { return 0; }
    out[0] = 0;
    int vol;
    size_t i;
    if (len >= 4 && memcmp(in, "lfs:", 4) == 0)      { vol = LFILE_LFS; i = 4; }
    else if (len >= 3 && memcmp(in, "sd:", 3) == 0)  { vol = LFILE_SD;  i = 3; }
    else { return 0; }

    while (i < len && in[i] == '/') { i++; }          /* lfs:/a == lfs:a */
    size_t end = len;
    while (end > i && in[end - 1] == '/') { end--; }  /* a trailing slash names the same thing */
    size_t n = end - i;
    if (n > LFILE_PATH_MAX || n + 1 > cap) { return 0; }

    size_t seg = 0;              /* length of the segment so far */
    size_t seg_start = 0;
    for (size_t k = 0; k < n; k++) {
        unsigned char c = (unsigned char)in[i + k];
        if (c == '/') {
            if (seg == 0) { return 0; }                    /* a//b */
            seg = 0;
            seg_start = k + 1;
            out[k] = '/';
            continue;
        }
        if (c == 0) { return 0; }
        if (vol == LFILE_LFS ? !lfs_char(c) : !sd_char(c)) { return 0; }
        if (vol == LFILE_LFS && seg == 0 && c == '.') { return 0; }   /* fs_valid_path's rule */
        if (++seg > LFILE_SEG_MAX) { return 0; }
        out[k] = (char)c;
        /* "." and ".." are ways out of the tree, never names. */
        if (k + 1 == n || in[i + k + 1] == '/') {
            size_t sl = k + 1 - seg_start;
            if ((sl == 1 && out[seg_start] == '.') ||
                (sl == 2 && out[seg_start] == '.' && out[seg_start + 1] == '.')) {
                return 0;
            }
        }
    }
    out[n] = 0;
    return vol;
}
