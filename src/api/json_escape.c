/*
 * JSON string escaping, alone in a file so it can be linked into a host test.
 *
 * It lived in api.c, which pulls in the filesystem, the transports and the
 * pico headers; nothing that wants to check the escaping needs any of that.
 * Splitting it keeps ONE escape implementation -- the point of the function is
 * that device names and script errors have exactly one way out -- while still
 * letting test/host link it.
 */
#include <stdio.h>

#include "api/api.h"

size_t api_json_escape(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    for (; *src && n + 7 < cap; src++) {
        unsigned char ch = (unsigned char)*src;
        switch (ch) {
            case '"':  n += (size_t)snprintf(dst + n, cap - n, "\\\""); break;
            case '\\': n += (size_t)snprintf(dst + n, cap - n, "\\\\"); break;
            case '\n': n += (size_t)snprintf(dst + n, cap - n, "\\n");  break;
            case '\r': n += (size_t)snprintf(dst + n, cap - n, "\\r");  break;
            case '\t': n += (size_t)snprintf(dst + n, cap - n, "\\t");  break;
            default:
                if (ch < 0x20) {
                    n += (size_t)snprintf(dst + n, cap - n, "\\u%04x", ch);
                } else {
                    dst[n++] = (char)ch;
                }
        }
    }
    if (n < cap) {
        dst[n] = 0;
    }
    return n;
}
