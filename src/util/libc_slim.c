/*
 * Smaller stand-ins for newlib functions whose stock versions cost far more
 * flash than this firmware uses them for.
 *
 * iswspace: newlib's vfscanf calls it (for %lc / %ls, which nothing here
 * uses), and its locale-aware version pulls in the JIS-to-Unicode table
 * (jp2uc, ~14 KB) and the Unicode category table (~14 KB). Every sscanf in
 * the firmware therefore cost 28 KB of flash for wide-character support it
 * never exercises. Defining the symbol here keeps the linker from pulling
 * iswspace.o out of libc at all; the ASCII answer is the correct one for
 * every character a %s/%d conversion can meet in this firmware's input.
 * Measured: -27.9 KB on the SD build (2026-10-20), which is what made room
 * for mbedTLS in its 864 KiB slot.
 */
#include <wctype.h>

int iswspace(wint_t c) {
    return c == ' ' || (c >= '\t' && c <= '\r');
}
