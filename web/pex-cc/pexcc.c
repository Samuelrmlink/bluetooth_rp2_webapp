/* pexcc -- a freestanding Thumb-2 subset compiler and PEX model-A linker.
 *
 * A full wasm clang/wasi toolchain is not available on this machine. Host
 * clang (wasm32-unknown-unknown) compiles THIS file to pexcc.wasm; the page
 * fetches that wasm, the import stub and libgcc from the SD card and runs
 * them. The compiler really compiles the module: it is not a canned ELF.
 *
 * Subset aimed at a PEX module: freestanding C, word-sized values, no locals,
 * no loops, calls through the import veneer. The linker consumes the same
 * relocatable ARM ELF gcc emits (SHT_REL, --emit-relocs layout of
 * pex/sdk/ld/pex_m33_reloc.ld) plus a GNU ar libgcc.
 *
 * Build the cache: web/pex-cc/make_pexcc.sh
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#ifdef PEXCC_WASM
void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *dd = d; const unsigned char *ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}
void *memset(void *d, int c, size_t n) {
    unsigned char *dd = d; while (n--) *dd++ = (unsigned char)c; return d;
}
void *memmove(void *d, const void *s, size_t n) {
    unsigned char *dd = d; const unsigned char *ss = s;
    if (dd == ss || !n) return d;
    if (dd < ss) { while (n--) *dd++ = *ss++; }
    else { dd += n; ss += n; while (n--) *--dd = *--ss; }
    return d;
}
int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    while (n--) { if (*x != *y) return (int)*x - (int)*y; x++; y++; }
    return 0;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (!n) return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
char *strchr(const char *s, int c) {
    unsigned char k = (unsigned char)c;
    while (*s) { if ((unsigned char)*s == k) return (char *)s; s++; }
    return k == 0 ? (char *)s : NULL;
}
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#endif

static char errbuf[512];
static int failed;

static void die(const char *fmt, ...) {
    va_list ap; const char *p; char *o, *end;
    if (failed) return;
    failed = 1;
    va_start(ap, fmt);
    o = errbuf; end = errbuf + sizeof errbuf - 1;
    for (p = fmt; *p && o < end; p++) {
        if (*p != '%') { *o++ = *p; continue; }
        p++;
        if (*p == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s && o < end) *o++ = *s++;
        } else if (*p == 'd' || *p == 'u' || *p == 'x') {
            char tmp[16]; int n = 0, neg = 0;
            unsigned v;
            if (*p == 'd') {
                int sv = va_arg(ap, int);
                if (sv < 0) { neg = 1; v = (unsigned)(-sv); } else v = (unsigned)sv;
            } else v = va_arg(ap, unsigned);
            if (*p == 'x') {
                const char *h = "0123456789abcdef";
                if (!v) tmp[n++] = '0';
                else while (v) { tmp[n++] = h[v & 15]; v >>= 4; }
            } else {
                if (!v) tmp[n++] = '0';
                else while (v) { tmp[n++] = '0' + (v % 10); v /= 10; }
            }
            if (neg && o < end) *o++ = '-';
            while (n && o < end) *o++ = tmp[--n];
        } else { *o++ = *p ? *p : '%'; if (!*p) break; }
    }
    *o = 0;
    va_end(ap);
}

static char arena[2 * 1024 * 1024];
static uint32_t arena_used;

static void *aalloc(uint32_t n) {
    uint32_t a;
    n = (n + 7u) & ~7u;
    if (failed) return NULL;
    if (arena_used + n > sizeof arena) { die("compiler out of memory"); return NULL; }
    a = arena_used; arena_used += n;
    memset(arena + a, 0, n);
    return arena + a;
}
static char *adup(const char *s, uint32_t n) {
    char *p = aalloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n); p[n] = 0;
    return p;
}
static char *adupz(const char *s) { return s ? adup(s, (uint32_t)strlen(s)) : NULL; }

/* ---------------------------------------------------------------- files */

enum { FK_H = 1, FK_O = 2, FK_A = 3, MAX_FILES = 48 };
static uint8_t files_mem[4 * 1024 * 1024];
static uint32_t files_used;

typedef struct {
    char name[96];
    const uint8_t *data;
    uint32_t size;
    int kind;
} FileRec;
static FileRec files[MAX_FILES];
static int nfiles;

#ifndef PEXCC_WASM
static char incdir[16][256];
static int nincdir;
#endif

static int file_kind(const char *name) {
    size_t n = strlen(name);
    if (n >= 2 && strcmp(name + n - 2, ".h") == 0) return FK_H;
    if (n >= 2 && strcmp(name + n - 2, ".o") == 0) return FK_O;
    if (n >= 2 && strcmp(name + n - 2, ".a") == 0) return FK_A;
    if (n >= 2 && strcmp(name + n - 2, ".c") == 0) return 0;
    return FK_H;
}

static int add_file_mem(const char *name, const uint8_t *data, uint32_t len) {
    FileRec *f;
    if (nfiles >= MAX_FILES) { die("too many toolchain files"); return 1; }
    f = &files[nfiles++];
    memset(f, 0, sizeof *f);
    if (strlen(name) >= sizeof f->name) { die("file name too long"); return 1; }
    memcpy(f->name, name, strlen(name) + 1);
    f->data = data; f->size = len; f->kind = file_kind(name);
    return 0;
}

static const uint8_t *find_header(const char *name, uint32_t *sz) {
    size_t nl = strlen(name);
    int i;
    for (i = 0; i < nfiles; i++) {
        size_t fl;
        if (files[i].kind != FK_H) continue;
        fl = strlen(files[i].name);
        if (strcmp(files[i].name, name) == 0 ||
            (fl >= nl && strcmp(files[i].name + fl - nl, name) == 0 &&
             (fl == nl || files[i].name[fl - nl - 1] == '/'))) {
            *sz = files[i].size;
            return files[i].data;
        }
    }
#ifndef PEXCC_WASM
    for (i = 0; i < nincdir; i++) {
        char path[512];
        FILE *fp; long n; uint8_t *buf;
        snprintf(path, sizeof path, "%s/%s", incdir[i], name);
        fp = fopen(path, "rb");
        if (!fp) continue;
        fseek(fp, 0, SEEK_END); n = ftell(fp); fseek(fp, 0, SEEK_SET);
        if (n < 0 || n > 256 * 1024) { fclose(fp); die("header too large"); return NULL; }
        if (files_used + (uint32_t)n + 1 > sizeof files_mem) { fclose(fp); die("file memory full"); return NULL; }
        buf = files_mem + files_used;
        if (fread(buf, 1, (size_t)n, fp) != (size_t)n) { fclose(fp); die("read failed"); return NULL; }
        fclose(fp);
        buf[n] = 0; files_used += (uint32_t)n + 1;
        add_file_mem(name, buf, (uint32_t)n);
        *sz = (uint32_t)n;
        return buf;
    }
#endif
    return NULL;
}

/* ---------------------------------------------------------------- preprocessor */

enum { MAX_MAC = 1024 };
typedef struct {
    const char *name;
    int is_fn, nparams, busy;
    const char *params[8];
    const char *body;
} Macro;
static Macro macros[MAX_MAC];
static int nmac;

static Macro *find_mac(const char *name) {
    int i;
    for (i = 0; i < nmac; i++) if (strcmp(macros[i].name, name) == 0) return &macros[i];
    return NULL;
}
static void add_obj_mac(const char *name, const char *body) {
    Macro *m = find_mac(name);
    if (!m) {
        if (nmac >= MAX_MAC) { die("too many macros"); return; }
        m = &macros[nmac++];
    }
    memset(m, 0, sizeof *m);
    m->name = adupz(name); m->body = body ? adupz(body) : adupz("");
}

static int is_id0(int c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
static int is_id1(int c) { return is_id0(c) || (c >= '0' && c <= '9'); }

static char stage[160 * 1024];
static char ppout[160 * 1024];
static uint32_t pplen;

static uint32_t splice_and_strip(const char *in, uint32_t n, char *out, uint32_t cap) {
    uint32_t i = 0, o = 0;
    while (i < n) {
        char c = in[i];
        if (c == '\\' && i + 1 < n && (in[i + 1] == '\n' || (in[i + 1] == '\r' && i + 2 < n && in[i + 2] == '\n'))) {
            i += (in[i + 1] == '\r') ? 3 : 2;
            continue;
        }
        if (c == '\r') { i++; continue; }
        if (c == '/' && i + 1 < n && in[i + 1] == '/') {
            i += 2;
            while (i < n && in[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < n && in[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(in[i] == '*' && in[i + 1] == '/')) {
                if (in[i] == '\n' && o + 1 < cap) out[o++] = '\n';
                i++;
            }
            i += (i + 1 < n) ? 2 : 0;
            if (o + 1 < cap) out[o++] = ' ';
            continue;
        }
        if (c == '"' || c == '\'') {
            char q = c;
            if (o + 1 >= cap) { die("header too large"); return 0; }
            out[o++] = in[i++];
            while (i < n && in[i] != q) {
                if (in[i] == '\\' && i + 1 < n) {
                    if (o + 2 >= cap) { die("header too large"); return 0; }
                    out[o++] = in[i++]; out[o++] = in[i++];
                } else {
                    if (o + 1 >= cap) { die("header too large"); return 0; }
                    if (in[i] == '\n' && q == '"') break;
                    out[o++] = in[i++];
                }
            }
            if (i < n && in[i] == q) { if (o + 1 < cap) out[o++] = in[i]; i++; }
            continue;
        }
        if (o + 1 >= cap) { die("header too large"); return 0; }
        out[o++] = c; i++;
    }
    if (o < cap) out[o] = 0;
    return o;
}

static int mac_defined(const char *name) { return find_mac(name) != NULL; }

/* Expand identifiers in s into out. hide is the macro currently being expanded. */
static void expand_to(const char *s, char *out, uint32_t cap, int depth);

static int ident_at(const char *s, uint32_t *len) {
    uint32_t n = 0;
    if (!is_id0((unsigned char)s[0])) return 0;
    while (is_id1((unsigned char)s[n])) n++;
    *len = n;
    return 1;
}

static void subst_fn(Macro *m, char args[][256], int nargs, char *out, uint32_t cap) {
    const char *b = m->body; uint32_t o = 0;
    while (*b && o + 1 < cap) {
        uint32_t n = 0; int pi;
        if (is_id0((unsigned char)*b) && ident_at(b, &n)) {
            for (pi = 0; pi < m->nparams; pi++) {
                if (strlen(m->params[pi]) == n && strncmp(b, m->params[pi], n) == 0) {
                    const char *a = pi < nargs ? args[pi] : "";
                    while (*a && o + 1 < cap) out[o++] = *a++;
                    b += n; goto next;
                }
            }
            if (o + n >= cap) break;
            memcpy(out + o, b, n); o += n; b += n;
            continue;
        }
        out[o++] = *b++;
        next: ;
    }
    out[o] = 0;
}

static void expand_to(const char *s, char *out, uint32_t cap, int depth) {
    uint32_t o = 0;
    if (depth > 40) { die("macro expansion too deep"); out[0] = 0; return; }
    while (*s && o + 1 < cap && !failed) {
        uint32_t n = 0;
        if (*s == '"') {
            out[o++] = *s++;
            while (*s && *s != '"') {
                if (*s == '\\' && s[1]) { if (o + 2 < cap) { out[o++] = *s++; out[o++] = *s++; } else break; }
                else { if (o + 1 < cap) out[o++] = *s++; else break; }
            }
            if (*s == '"' && o + 1 < cap) out[o++] = *s++;
            continue;
        }
        if (is_id0((unsigned char)*s) && ident_at(s, &n)) {
            char name[64];
            Macro *m;
            if (n >= sizeof name) n = sizeof name - 1;
            memcpy(name, s, n); name[n] = 0;
            m = find_mac(name);
            if (!m || m->busy) {
                if (o + n >= cap) break;
                memcpy(out + o, s, n); o += n; s += n;
                continue;
            }
            s += n;
            if (m->is_fn) {
                const char *q = s;
                while (*q == ' ' || *q == '\t') q++;
                if (*q != '(') {
                    if (o + n >= cap) break;
                    memcpy(out + o, name, n); o += n;
                    continue;
                }
                {
                    char args[8][256];
                    int nargs = 0, depthp = 0;
                    const char *a = q + 1;
                    uint32_t al = 0;
                    memset(args, 0, sizeof args);
                    while (*a && !failed) {
                        if (*a == '"') {
                            if (nargs < 8 && al + 1 < 256) args[nargs][al++] = *a;
                            a++;
                            while (*a && *a != '"') {
                                if (nargs < 8 && al + 1 < 256) args[nargs][al++] = *a;
                                if (*a == '\\' && a[1]) { a++; if (nargs < 8 && al + 1 < 256) args[nargs][al++] = *a; }
                                a++;
                            }
                            if (*a == '"') { if (nargs < 8 && al + 1 < 256) args[nargs][al++] = *a; a++; }
                            continue;
                        }
                        if (*a == '(') { depthp++; if (nargs < 8 && al + 1 < 256) args[nargs][al++] = *a++; continue; }
                        if (*a == ')') {
                            if (depthp == 0) { a++; break; }
                            depthp--; if (nargs < 8 && al + 1 < 256) args[nargs][al++] = *a++; continue;
                        }
                        if (*a == ',' && depthp == 0) {
                            if (nargs < 8) args[nargs][al] = 0;
                            nargs++; al = 0; a++;
                            while (*a == ' ' || *a == '\t') a++;
                            continue;
                        }
                        if (nargs < 8 && al + 1 < 256) args[nargs][al++] = *a;
                        a++;
                    }
                    if (al || nargs) { if (nargs < 8) args[nargs][al] = 0; nargs++; }
                    {
                        char sub[1024], exp[1024];
                        m->busy = 1;
                        subst_fn(m, args, nargs, sub, sizeof sub);
                        expand_to(sub, exp, sizeof exp, depth + 1);
                        m->busy = 0;
                        {
                            uint32_t el = (uint32_t)strlen(exp);
                            if (o + el >= cap) el = cap - o - 1;
                            memcpy(out + o, exp, el); o += el;
                        }
                    }
                    s = a;
                }
                continue;
            }
            {
                char exp[1024];
                m->busy = 1;
                expand_to(m->body ? m->body : "", exp, sizeof exp, depth + 1);
                m->busy = 0;
                {
                    uint32_t el = (uint32_t)strlen(exp);
                    if (o + el >= cap) el = cap - o - 1;
                    memcpy(out + o, exp, el); o += el;
                }
            }
            continue;
        }
        out[o++] = *s++;
    }
    out[o] = 0;
}

static int64_t eval_expr(const char **ps);

static void skip_ws(const char **ps) { while (**ps == ' ' || **ps == '\t') (*ps)++; }

static int64_t eval_primary(const char **ps) {
    int64_t v; int neg = 0;
    skip_ws(ps);
    if (**ps == '(') { (*ps)++; v = eval_expr(ps); skip_ws(ps); if (**ps == ')') (*ps)++; return v; }
    if (**ps == '+') { (*ps)++; return eval_primary(ps); }
    if (**ps == '-') { (*ps)++; return -eval_primary(ps); }
    if (**ps == '!') { (*ps)++; return !eval_primary(ps); }
    if (**ps == '~') { (*ps)++; return ~eval_primary(ps); }
    if (**ps == '\'') {
        (*ps)++;
        v = (unsigned char)**ps;
        if (**ps == '\\') { (*ps)++; v = **ps == 'n' ? '\n' : **ps == '0' ? 0 : **ps; }
        if (**ps) (*ps)++;
        if (**ps == '\'') (*ps)++;
        return v;
    }
    if (is_id0((unsigned char)**ps)) {
        /* leftover identifiers are 0 (defined() already rewritten) */
        while (is_id1((unsigned char)**ps)) (*ps)++;
        return 0;
    }
    if (**ps == '0' && ((*ps)[1] == 'x' || (*ps)[1] == 'X')) {
        *ps += 2; v = 0;
        while ((**ps >= '0' && **ps <= '9') || (**ps >= 'a' && **ps <= 'f') || (**ps >= 'A' && **ps <= 'F')) {
            v *= 16;
            v += (**ps <= '9') ? **ps - '0' : (**ps & ~32) - 'A' + 10;
            (*ps)++;
        }
    } else if (**ps >= '0' && **ps <= '9') {
        v = 0;
        if (**ps == '0') neg = 1; /* maybe octal */
        while (**ps >= '0' && **ps <= '9') { v = v * 10 + (**ps - '0'); (*ps)++; }
        (void)neg;
    } else return 0;
    while (**ps == 'u' || **ps == 'U' || **ps == 'l' || **ps == 'L') (*ps)++;
    return v;
}

static int64_t eval_mul(const char **ps) {
    int64_t v = eval_primary(ps);
    for (;;) {
        skip_ws(ps);
        if (**ps == '*' && (*ps)[1] != '=') { (*ps)++; v *= eval_primary(ps); }
        else if (**ps == '/' && (*ps)[1] != '=') { (*ps)++; { int64_t d = eval_primary(ps); v = d ? v / d : 0; } }
        else if (**ps == '%' && (*ps)[1] != '=') { (*ps)++; { int64_t d = eval_primary(ps); v = d ? v % d : 0; } }
        else break;
    }
    return v;
}
static int64_t eval_add(const char **ps) {
    int64_t v = eval_mul(ps);
    for (;;) {
        skip_ws(ps);
        if (**ps == '+' && (*ps)[1] != '=' && (*ps)[1] != '+') { (*ps)++; v += eval_mul(ps); }
        else if (**ps == '-' && (*ps)[1] != '=' && (*ps)[1] != '-') { (*ps)++; v -= eval_mul(ps); }
        else break;
    }
    return v;
}
static int64_t eval_shift(const char **ps) {
    int64_t v = eval_add(ps);
    for (;;) {
        skip_ws(ps);
        if (**ps == '<' && (*ps)[1] == '<') { *ps += 2; v <<= eval_add(ps); }
        else if (**ps == '>' && (*ps)[1] == '>') { *ps += 2; v >>= eval_add(ps); }
        else break;
    }
    return v;
}
static int64_t eval_rel(const char **ps) {
    int64_t v = eval_shift(ps);
    for (;;) {
        skip_ws(ps);
        if (**ps == '<' && (*ps)[1] == '=') { *ps += 2; v = v <= eval_shift(ps); }
        else if (**ps == '>' && (*ps)[1] == '=') { *ps += 2; v = v >= eval_shift(ps); }
        else if (**ps == '<' && (*ps)[1] != '<') { (*ps)++; v = v < eval_shift(ps); }
        else if (**ps == '>' && (*ps)[1] != '>') { (*ps)++; v = v > eval_shift(ps); }
        else break;
    }
    return v;
}
static int64_t eval_eq(const char **ps) {
    int64_t v = eval_rel(ps);
    for (;;) {
        skip_ws(ps);
        if (**ps == '=' && (*ps)[1] == '=') { *ps += 2; v = v == eval_rel(ps); }
        else if (**ps == '!' && (*ps)[1] == '=') { *ps += 2; v = v != eval_rel(ps); }
        else break;
    }
    return v;
}
static int64_t eval_band(const char **ps) {
    int64_t v = eval_eq(ps);
    for (;;) { skip_ws(ps); if (**ps == '&' && (*ps)[1] != '&') { (*ps)++; v &= eval_eq(ps); } else break; }
    return v;
}
static int64_t eval_bxor(const char **ps) {
    int64_t v = eval_band(ps);
    for (;;) { skip_ws(ps); if (**ps == '^') { (*ps)++; v ^= eval_band(ps); } else break; }
    return v;
}
static int64_t eval_bor(const char **ps) {
    int64_t v = eval_bxor(ps);
    for (;;) { skip_ws(ps); if (**ps == '|' && (*ps)[1] != '|') { (*ps)++; v |= eval_bxor(ps); } else break; }
    return v;
}
static int64_t eval_land(const char **ps) {
    int64_t v = eval_bor(ps);
    for (;;) { skip_ws(ps); if (**ps == '&' && (*ps)[1] == '&') { *ps += 2; v = eval_bor(ps) && v; } else break; }
    return v;
}
static int64_t eval_lor(const char **ps) {
    int64_t v = eval_land(ps);
    for (;;) { skip_ws(ps); if (**ps == '|' && (*ps)[1] == '|') { *ps += 2; v = eval_land(ps) || v; } else break; }
    return v;
}
static int64_t eval_expr(const char **ps) { return eval_lor(ps); }

/* Rewrite defined(X) / defined X to 1 or 0, then expand and evaluate. */
static int eval_if_line(const char *line) {
    char buf[1024], exp[1024];
    uint32_t o = 0;
    const char *s = line;
    while (*s && o + 1 < sizeof buf) {
        uint32_t n = 0;
        if (is_id0((unsigned char)*s) && ident_at(s, &n) && n == 7 && strncmp(s, "defined", 7) == 0) {
            const char *q = s + 7; int paren = 0; char name[64]; uint32_t nl = 0;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '(') { paren = 1; q++; while (*q == ' ' || *q == '\t') q++; }
            while (is_id1((unsigned char)*q) && nl + 1 < sizeof name) name[nl++] = *q++;
            name[nl] = 0;
            if (paren) { while (*q == ' ' || *q == '\t') q++; if (*q == ')') q++; }
            buf[o++] = mac_defined(name) ? '1' : '0';
            s = q;
            continue;
        }
        buf[o++] = *s++;
    }
    buf[o] = 0;
    expand_to(buf, exp, sizeof exp, 0);
    s = exp;
    return eval_expr(&s) != 0;
}

enum { IFMAX = 32 };
typedef struct { int active, taken, parent; } IfFrame;
static IfFrame ifs[IFMAX];
static int nif;

static int pp_active(void) { return nif == 0 || ifs[nif - 1].active; }

static void pp_emit(const char *s) {
    uint32_t n = (uint32_t)strlen(s);
    if (pplen + n + 2 >= sizeof ppout) { die("preprocessed source too large"); return; }
    memcpy(ppout + pplen, s, n); pplen += n;
    ppout[pplen++] = '\n'; ppout[pplen] = 0;
}

static const char *skip_sp(const char *s) { while (*s == ' ' || *s == '\t') s++; return s; }

static void define_macro(const char *rest) {
    char name[64]; uint32_t n = 0; int is_fn = 0;
    const char *s = skip_sp(rest);
    if (!is_id0((unsigned char)*s)) { die("bad #define"); return; }
    while (is_id1((unsigned char)*s) && n + 1 < sizeof name) name[n++] = *s++;
    name[n] = 0;
    if (*s == '(') {
        Macro *m; int np = 0;
        is_fn = 1; s++;
        if (nmac >= MAX_MAC) { die("too many macros"); return; }
        m = find_mac(name);
        if (!m) m = &macros[nmac++];
        memset(m, 0, sizeof *m);
        m->name = adupz(name); m->is_fn = 1;
        s = skip_sp(s);
        if (*s != ')') {
            while (*s && *s != ')' && np < 8) {
                char pnm[32]; uint32_t pn = 0;
                s = skip_sp(s);
                if (*s == '.' && s[1] == '.' && s[2] == '.') { s += 3; break; }
                while (is_id1((unsigned char)*s) && pn + 1 < sizeof pnm) pnm[pn++] = *s++;
                pnm[pn] = 0;
                m->params[np++] = adupz(pnm);
                s = skip_sp(s);
                if (*s == ',') s++;
            }
        }
        m->nparams = np;
        if (*s == ')') s++;
        s = skip_sp(s);
        m->body = adupz(s);
        return;
    }
    s = skip_sp(s);
    add_obj_mac(name, s);
    (void)is_fn;
}

static void pp_lines(char *text, const char *fname, int depth) {
    char *p = text;
    int line = 1;
    if (depth > 24) { die("include nesting too deep"); return; }
    while (*p && !failed) {
        char *nl = strchr(p, '\n');
        char *s;
        if (nl) *nl = 0;
        s = (char *)skip_sp(p);
        if (*s == '#') {
            const char *d = skip_sp(s + 1);
            int cond = 0;
            if (strncmp(d, "if", 2) == 0 && (d[2] == ' ' || d[2] == '\t' || d[2] == '(' || strncmp(d, "ifdef", 5) == 0 || strncmp(d, "ifndef", 6) == 0))
                cond = 1;
            if (strncmp(d, "ifdef", 5) == 0 || strncmp(d, "ifndef", 6) == 0 ||
                (d[0] == 'i' && d[1] == 'f' && (d[2] == ' ' || d[2] == '\t' || d[2] == '(')) ||
                strncmp(d, "elif", 4) == 0 || strncmp(d, "else", 4) == 0 || strncmp(d, "endif", 5) == 0)
                cond = 1;
            if (cond) {
                if (strncmp(d, "endif", 5) == 0) {
                    if (nif) nif--;
                } else if (strncmp(d, "else", 4) == 0 && (d[4] == 0 || d[4] == ' ' || d[4] == '\t')) {
                    if (nif) {
                        IfFrame *f = &ifs[nif - 1];
                        f->active = f->parent && !f->taken;
                        f->taken = 1;
                    }
                } else if (strncmp(d, "elif", 4) == 0) {
                    if (nif) {
                        IfFrame *f = &ifs[nif - 1];
                        if (!f->parent || f->taken) f->active = 0;
                        else {
                            int on = eval_if_line(skip_sp(d + 4));
                            f->active = on; f->taken = on;
                        }
                    }
                } else if (nif >= IFMAX) die("#if too deep");
                else {
                    IfFrame *f = &ifs[nif];
                    int parent = pp_active();
                    /* parent already includes current frame; a new frame's parent is the current active */
                    f->parent = parent;
                    if (!parent) { f->active = 0; f->taken = 1; }
                    else if (strncmp(d, "ifdef", 5) == 0) {
                        char name[64]; uint32_t n = 0; const char *q = skip_sp(d + 5);
                        while (is_id1((unsigned char)*q) && n + 1 < sizeof name) name[n++] = *q++;
                        name[n] = 0;
                        f->active = mac_defined(name); f->taken = f->active;
                    } else if (strncmp(d, "ifndef", 6) == 0) {
                        char name[64]; uint32_t n = 0; const char *q = skip_sp(d + 6);
                        while (is_id1((unsigned char)*q) && n + 1 < sizeof name) name[n++] = *q++;
                        name[n] = 0;
                        f->active = !mac_defined(name); f->taken = f->active;
                    } else {
                        int on = eval_if_line(skip_sp(d + 2));
                        f->active = on; f->taken = on;
                    }
                    nif++;
                }
            } else if (pp_active()) {
                if (strncmp(d, "define", 6) == 0) define_macro(d + 6);
                else if (strncmp(d, "undef", 5) == 0) {
                    char name[64]; uint32_t n = 0; const char *q = skip_sp(d + 5);
                    int i;
                    while (is_id1((unsigned char)*q) && n + 1 < sizeof name) name[n++] = *q++;
                    name[n] = 0;
                    for (i = 0; i < nmac; i++) if (strcmp(macros[i].name, name) == 0) {
                        macros[i] = macros[nmac - 1]; nmac--; break;
                    }
                } else if (strncmp(d, "include", 7) == 0) {
                    const char *q = skip_sp(d + 7);
                    char iname[128]; uint32_t n = 0; char exp[256];
                    const uint8_t *data; uint32_t sz = 0; char *cleaned;
                    if (*q != '"' && *q != '<') {
                        expand_to(q, exp, sizeof exp, 0);
                        q = skip_sp(exp);
                    }
                    if (*q == '"' || *q == '<') {
                        char end = (*q == '"') ? '"' : '>';
                        q++;
                        while (*q && *q != end && n + 1 < sizeof iname) iname[n++] = *q++;
                        iname[n] = 0;
                        data = find_header(iname, &sz);
                        if (!data) { die("cannot find %s", iname); }
                        else {
                            cleaned = aalloc(sz + 1);
                            if (cleaned) {
                                splice_and_strip((const char *)data, sz, stage, sizeof stage);
                                memcpy(cleaned, stage, strlen(stage) + 1);
                                pp_lines(cleaned, iname, depth + 1);
                            }
                        }
                    } else die("bad #include");
                } else if (strncmp(d, "error", 5) == 0) die("#error %s", skip_sp(d + 5));
                else if (strncmp(d, "pragma", 6) == 0 || strncmp(d, "line", 4) == 0 || strncmp(d, "warning", 7) == 0) {
                    /* ignored */
                } else if (*d == 0) {
                    /* empty directive */
                } else die("unknown directive #%s", d);
            }
        } else if (pp_active() && *s) {
            char exp[2048];
            expand_to(s, exp, sizeof exp, 0);
            pp_emit(exp);
        }
        if (!nl) break;
        *nl = '\n'; p = nl + 1; line++;
        (void)line; (void)fname;
    }
}

static void preprocess(const char *src, uint32_t len) {
    char *cleaned;
    pplen = 0; ppout[0] = 0; nif = 0; nmac = 0;
    add_obj_mac("__GNUC__", "4");
    cleaned = aalloc(len + 1);
    if (!cleaned) return;
    splice_and_strip(src, len, stage, sizeof stage);
    memcpy(cleaned, stage, strlen(stage) + 1);
    pp_lines(cleaned, "module.c", 0);
}

/* ---------------------------------------------------------------- tokens */

enum { TK_EOF = 0, TK_IDENT = 1, TK_NUM = 2, TK_STR = 3, TK_PUNCT = 4 };
enum {
    PU_EQ = 256, PU_NE, PU_LE, PU_GE, PU_LAND, PU_LOR, PU_SHL, PU_SHR,
    PU_INC, PU_DEC, PU_ARROW, PU_ELLIPSIS
};
typedef struct {
    int kind, punct, line;
    int64_t num;
    const char *str;
    int len;
} Tok;
enum { MAX_TOK = 20000 };
static Tok toks[MAX_TOK];
static int ntok, ti;

static void push_tok(Tok t) {
    if (ntok >= MAX_TOK - 1) { die("too many tokens"); return; }
    toks[ntok++] = t;
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void tokenize(const char *s) {
    int line = 1;
    ntok = 0; ti = 0;
    while (*s && !failed) {
        Tok t; memset(&t, 0, sizeof t); t.line = line;
        if (*s == ' ' || *s == '\t' || *s == '\r') { s++; continue; }
        if (*s == '\n') { line++; s++; continue; }
        if (is_id0((unsigned char)*s)) {
            const char *b = s;
            while (is_id1((unsigned char)*s)) s++;
            t.kind = TK_IDENT; t.str = adup(b, (uint32_t)(s - b)); t.len = (int)(s - b);
            push_tok(t); continue;
        }
        if (*s >= '0' && *s <= '9') {
            int64_t v = 0;
            if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
                s += 2;
                while (hexval((unsigned char)*s) >= 0) { v = v * 16 + hexval((unsigned char)*s); s++; }
            } else if (s[0] == '0') {
                while (*s >= '0' && *s <= '7') { v = v * 8 + (*s - '0'); s++; }
            } else {
                while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
            }
            while (*s == 'u' || *s == 'U' || *s == 'l' || *s == 'L') s++;
            t.kind = TK_NUM; t.num = v; push_tok(t); continue;
        }
        if (*s == '"') {
            char buf[1024]; int n = 0;
            s++;
            while (*s && *s != '"') {
                char c = *s++;
                if (c == '\\' && *s) {
                    char e = *s++;
                    c = e == 'n' ? '\n' : e == 'r' ? '\r' : e == 't' ? '\t' : e == '0' ? 0 : e;
                }
                if (n + 1 < (int)sizeof buf) buf[n++] = c;
            }
            if (*s == '"') s++;
            t.kind = TK_STR; t.str = adup(buf, (uint32_t)n); t.len = n;
            push_tok(t); continue;
        }
        if (*s == '\'') {
            int c;
            s++; c = (unsigned char)*s;
            if (*s == '\\' && s[1]) { s++; c = *s == 'n' ? '\n' : *s == '0' ? 0 : (unsigned char)*s; s++; }
            else if (*s) s++;
            if (*s == '\'') s++;
            t.kind = TK_NUM; t.num = c; push_tok(t); continue;
        }
        t.kind = TK_PUNCT;
        if (s[0] == '=' && s[1] == '=') { t.punct = PU_EQ; s += 2; }
        else if (s[0] == '!' && s[1] == '=') { t.punct = PU_NE; s += 2; }
        else if (s[0] == '<' && s[1] == '=') { t.punct = PU_LE; s += 2; }
        else if (s[0] == '>' && s[1] == '=') { t.punct = PU_GE; s += 2; }
        else if (s[0] == '&' && s[1] == '&') { t.punct = PU_LAND; s += 2; }
        else if (s[0] == '|' && s[1] == '|') { t.punct = PU_LOR; s += 2; }
        else if (s[0] == '<' && s[1] == '<') { t.punct = PU_SHL; s += 2; }
        else if (s[0] == '>' && s[1] == '>') { t.punct = PU_SHR; s += 2; }
        else if (s[0] == '+' && s[1] == '+') { t.punct = PU_INC; s += 2; }
        else if (s[0] == '-' && s[1] == '-') { t.punct = PU_DEC; s += 2; }
        else if (s[0] == '-' && s[1] == '>') { t.punct = PU_ARROW; s += 2; }
        else if (s[0] == '.' && s[1] == '.' && s[2] == '.') { t.punct = PU_ELLIPSIS; s += 3; }
        else { t.punct = (unsigned char)*s++; }
        push_tok(t);
    }
    { Tok t; memset(&t, 0, sizeof t); t.kind = TK_EOF; push_tok(t); }
}

static int Tkind(void) { return ti < ntok ? toks[ti].kind : TK_EOF; }
static int Tp(void) { return ti < ntok ? toks[ti].punct : 0; }
static int see_p(int p) { return Tkind() == TK_PUNCT && Tp() == p; }
static int see_id(const char *s) {
    return Tkind() == TK_IDENT && toks[ti].str && strcmp(toks[ti].str, s) == 0;
}
static void bump(void) { if (ti < ntok && Tkind() != TK_EOF) ti++; }
static int eat_p(int p) { if (see_p(p)) { bump(); return 1; } return 0; }
static int eat_id(const char *s) { if (see_id(s)) { bump(); return 1; } return 0; }
static void expect_p(int p) {
    if (!eat_p(p)) die("expected '%c' (line %d)", p < 128 ? p : '?', ti < ntok ? toks[ti].line : 0);
}

/* ---------------------------------------------------------------- types / decls */

static const char *typedefs[768];
static int ntypedef;

static int is_typedef_name(const char *s) {
    int i; for (i = 0; i < ntypedef; i++) if (strcmp(typedefs[i], s) == 0) return 1;
    return 0;
}
static void add_typedef(const char *s) {
    if (!s || !*s || is_typedef_name(s)) return;
    if (ntypedef >= (int)(sizeof typedefs / sizeof typedefs[0])) { die("too many typedefs"); return; }
    typedefs[ntypedef++] = s;
}

static int is_type_kw(const char *s) {
    static const char *k[] = {
        "void","char","short","int","long","signed","unsigned","float","double",
        "_Bool","bool","struct","union","enum", NULL
    };
    int i; for (i = 0; k[i]; i++) if (strcmp(s, k[i]) == 0) return 1;
    return 0;
}
static int is_qual(const char *s) {
    return strcmp(s, "const") == 0 || strcmp(s, "volatile") == 0 || strcmp(s, "restrict") == 0 ||
           strcmp(s, "__restrict") == 0 || strcmp(s, "__restrict__") == 0;
}
static int is_storage(const char *s) {
    return strcmp(s, "typedef") == 0 || strcmp(s, "static") == 0 || strcmp(s, "extern") == 0 ||
           strcmp(s, "auto") == 0 || strcmp(s, "register") == 0 || strcmp(s, "inline") == 0 ||
           strcmp(s, "__inline") == 0 || strcmp(s, "__inline__") == 0;
}

static void skip_parens(void) {
    int d = 0;
    if (!see_p('(')) return;
    do {
        if (see_p('(')) d++;
        else if (see_p(')')) d--;
        bump();
    } while (d && Tkind() != TK_EOF && !failed);
}

static void skip_attr_if(void) {
    if (!(see_id("__attribute__") || see_id("__attribute") || see_id("__asm__") || see_id("__asm") || see_id("asm")))
        return;
    bump();
    skip_parens();
}

typedef struct {
    const char *name;
    int is_func, nparams, ellipsis, is_static;
    const char *params[4];
} Decl;

static void skip_expr_tok(void);
static void parse_decl_specs(int *is_typedef, int *is_static);
static void parse_declarator(Decl *d);

static int is_type_start(void) {
    if (Tkind() != TK_IDENT || !toks[ti].str) return 0;
    return is_type_kw(toks[ti].str) || is_qual(toks[ti].str) || is_storage(toks[ti].str) ||
           is_typedef_name(toks[ti].str) || strcmp(toks[ti].str, "__extension__") == 0 ||
           strcmp(toks[ti].str, "__attribute__") == 0;
}

static void parse_members(void) {
    expect_p('{');
    while (!see_p('}') && Tkind() != TK_EOF && !failed) {
        int td = 0, st = 0;
        if (eat_p(';')) continue;
        parse_decl_specs(&td, &st);
        if (eat_p(';')) continue;
        if (see_p(':')) { bump(); skip_expr_tok(); }
        else {
            for (;;) {
                Decl d; memset(&d, 0, sizeof d);
                if (!see_p(':') && !see_p(';') && !see_p('}')) parse_declarator(&d);
                if (eat_p(':')) skip_expr_tok();
                if (!eat_p(',')) break;
            }
        }
        expect_p(';');
    }
    expect_p('}');
}

static void parse_enum_body(void) {
    expect_p('{');
    while (!see_p('}') && Tkind() != TK_EOF && !failed) {
        if (Tkind() == TK_IDENT) bump();
        if (eat_p('=')) skip_expr_tok();
        if (!eat_p(',')) break;
    }
    expect_p('}');
}

static void parse_decl_specs(int *is_typedef, int *is_static) {
    int saw = 0, guard = 0;
    while (!failed && guard++ < 64) {
        int before = ti;
        skip_attr_if();
        if (see_id("__extension__")) { bump(); continue; }
        if (Tkind() == TK_IDENT && toks[ti].str && is_storage(toks[ti].str)) {
            if (strcmp(toks[ti].str, "typedef") == 0 && is_typedef) *is_typedef = 1;
            if (strcmp(toks[ti].str, "static") == 0 && is_static) *is_static = 1;
            bump(); continue;
        }
        if (Tkind() == TK_IDENT && toks[ti].str && is_qual(toks[ti].str)) { bump(); continue; }
        if (Tkind() == TK_IDENT && toks[ti].str &&
            (strcmp(toks[ti].str, "struct") == 0 || strcmp(toks[ti].str, "union") == 0)) {
            bump();
            if (Tkind() == TK_IDENT) bump();
            if (see_p('{')) parse_members();
            saw = 1; continue;
        }
        if (see_id("enum")) {
            bump();
            if (Tkind() == TK_IDENT) bump();
            if (see_p('{')) parse_enum_body();
            saw = 1; continue;
        }
        if (Tkind() == TK_IDENT && toks[ti].str && is_type_kw(toks[ti].str)) { bump(); saw = 1; continue; }
        if (!saw && Tkind() == TK_IDENT && toks[ti].str && is_typedef_name(toks[ti].str)) { bump(); saw = 1; continue; }
        if (ti == before) break;
    }
    if (!saw && !failed) die("expected a type (line %d)", ti < ntok ? toks[ti].line : 0);
}

static void parse_params(Decl *d) {
    expect_p('(');
    d->is_func = 1;
    d->nparams = 0;
    if (see_id("void") && ti + 1 < ntok && toks[ti + 1].kind == TK_PUNCT && toks[ti + 1].punct == ')') {
        bump(); expect_p(')'); return;
    }
    if (eat_p(')')) return;
    while (!failed) {
        int td = 0, st = 0;
        Decl pd;
        if (see_p(PU_ELLIPSIS)) { d->ellipsis = 1; bump(); break; }
        memset(&pd, 0, sizeof pd);
        parse_decl_specs(&td, &st);
        if (!see_p(')') && !see_p(',') && !see_p(PU_ELLIPSIS)) parse_declarator(&pd);
        if (pd.name && d->nparams < 4) d->params[d->nparams] = pd.name;
        if (pd.name || d->nparams < 8) d->nparams++;
        if (eat_p(',')) continue;
        break;
    }
    expect_p(')');
}

static void parse_direct(Decl *d) {
    if (Tkind() == TK_IDENT) { d->name = toks[ti].str; bump(); }
    else if (eat_p('(')) { parse_declarator(d); expect_p(')'); }
    while (!failed) {
        if (eat_p('[')) {
            if (!see_p(']')) skip_expr_tok();
            expect_p(']');
        } else if (see_p('(')) {
            Decl inner;
            /* A suffix parameter list belongs to this declarator. Nested
             * function-pointer parameters must not replace the outer name. */
            memset(&inner, 0, sizeof inner);
            inner.name = d->name;
            parse_params(&inner);
            d->is_func = 1;
            d->ellipsis = inner.ellipsis;
            d->nparams = inner.nparams < 4 ? inner.nparams : 4;
            {
                int i; for (i = 0; i < d->nparams && i < 4; i++) d->params[i] = inner.params[i];
            }
        } else break;
    }
}

static void parse_declarator(Decl *d) {
    while (eat_p('*')) {
        while (Tkind() == TK_IDENT && toks[ti].str && is_qual(toks[ti].str)) bump();
        skip_attr_if();
    }
    parse_direct(d);
    skip_attr_if();
}

static void skip_expr_tok(void) {
    int d = 0;
    while (Tkind() != TK_EOF && !failed) {
        int p = Tp();
        if (Tkind() == TK_PUNCT && d == 0 &&
            (p == ')' || p == ']' || p == ',' || p == ';' || p == '}' || p == ':')) break;
        if (Tkind() == TK_PUNCT && (p == '(' || p == '[' || p == '{')) d++;
        if (Tkind() == TK_PUNCT && (p == ')' || p == ']' || p == '}')) { if (d) d--; }
        bump();
    }
}

static void skip_initializer(void) {
    int d = 0;
    if (!eat_p('=')) return;
    while (Tkind() != TK_EOF && !failed) {
        int p = Tp();
        if (Tkind() == TK_PUNCT && (p == '{' || p == '(')) { d++; bump(); continue; }
        if (Tkind() == TK_PUNCT && (p == '}' || p == ')')) {
            if (d == 0) break;
            d--; bump(); continue;
        }
        if (d == 0 && Tkind() == TK_PUNCT && (p == ',' || p == ';')) break;
        bump();
    }
}

/* ---------------------------------------------------------------- codegen */

enum { REL_CALL = 1, REL_RO = 2 };
typedef struct { uint32_t off; int kind, sym; } CRel;
typedef struct { const char *name; uint32_t value, size; int bind; /* 0 undef, 1 global, 2 local */ int defined; } CSym;
enum { MAX_CREL = 1024, MAX_CSYM = 256, MAX_LAB = 256, MAX_BR = 512, MAX_LIT = 128 };

static uint8_t ctext[64 * 1024];
static uint32_t ctlen;
static uint8_t cro[32 * 1024];
static uint32_t crolen;
static CRel crels[MAX_CREL];
static int ncrel;
static CSym csyms[MAX_CSYM];
static int ncsym;

static int labels[MAX_LAB];
static int nlab;
static struct { int at, lab, cond; } brs[MAX_BR];
static int nbr;
static struct { int ldr; int kind; uint32_t val; } lits[MAX_LIT];
static int nlit, lit0;
static int epi_lab;

static void e16(uint16_t w) {
    if (ctlen + 2 > sizeof ctext) { die("function text too large"); return; }
    ctext[ctlen++] = (uint8_t)(w & 255);
    ctext[ctlen++] = (uint8_t)(w >> 8);
}
static int new_lab(void) {
    if (nlab >= MAX_LAB) { die("too many labels"); return 0; }
    labels[nlab] = -1; return nlab++;
}
static void bind_lab(int id) { if (id >= 0 && id < nlab) labels[id] = (int)ctlen; }
static void emit_b(int cond, int lab) {
    if (nbr >= MAX_BR) { die("too many branches"); return; }
    brs[nbr].at = (int)ctlen; brs[nbr].lab = lab; brs[nbr].cond = cond; nbr++;
    e16(0);
}
static void patch_brs(void) {
    int i;
    for (i = 0; i < nbr; i++) {
        int target = labels[brs[i].lab];
        int32_t imm;
        if (target < 0) { die("unbound branch"); return; }
        imm = (target - (brs[i].at + 4)) >> 1;
        if (brs[i].cond == 14) {
            if (imm < -1024 || imm > 1023) { die("branch out of range"); return; }
            ctext[brs[i].at] = (uint8_t)(imm & 255);
            ctext[brs[i].at + 1] = (uint8_t)(0xE0 | ((imm >> 8) & 7));
        } else {
            if (imm < -128 || imm > 127) { die("branch out of range"); return; }
            ctext[brs[i].at] = (uint8_t)(imm & 255);
            ctext[brs[i].at + 1] = (uint8_t)(0xD0 | brs[i].cond);
        }
    }
}
static void mov_rr(int rd, int rm) {
    e16((uint16_t)(0x4600 | ((rm & 8) << 4) | ((rd & 8) << 3) | ((rm & 7) << 3) | (rd & 7)));
}
static void movs_imm(int rd, int imm) { e16((uint16_t)(0x2000 | (rd << 8) | (imm & 255))); }
static void sub_sp(int bytes) { e16((uint16_t)(0xB080 | (bytes >> 2))); }
static void add_sp(int bytes) { e16((uint16_t)(0xB000 | (bytes >> 2))); }
static void str_sp(int rt, int imm) { e16(0xF8CD); e16((uint16_t)((rt << 12) | imm)); }
static void ldr_sp(int rt, int imm) { e16(0xF8DD); e16((uint16_t)((rt << 12) | imm)); }

static int csym_find(const char *name) {
    int i; for (i = 0; i < ncsym; i++) if (strcmp(csyms[i].name, name) == 0) return i;
    return -1;
}
static int csym_undef(const char *name) {
    int i = csym_find(name);
    if (i >= 0) return i;
    if (ncsym >= MAX_CSYM) { die("too many symbols"); return 0; }
    csyms[ncsym].name = name; csyms[ncsym].defined = 0; csyms[ncsym].bind = 1;
    return ncsym++;
}
static void emit_bl(const char *name) {
    int sym = csym_undef(name);
    if (ncrel >= MAX_CREL) { die("too many relocs"); return; }
    crels[ncrel].off = ctlen; crels[ncrel].kind = REL_CALL; crels[ncrel].sym = sym; ncrel++;
    /* unresolved BL: hw1=0xF7FF hw2=0xFFFE, addend -4 */
    ctext[ctlen++] = 0xff; ctext[ctlen++] = 0xf7; ctext[ctlen++] = 0xfe; ctext[ctlen++] = 0xff;
    if (ctlen > sizeof ctext) die("text overflow");
}

static void emit_setcc(int cond) {
    int ly = new_lab(), ln = new_lab();
    emit_b(cond, ly);
    movs_imm(0, 0);
    emit_b(14, ln);
    bind_lab(ly);
    movs_imm(0, 1);
    bind_lab(ln);
}

static const char *cur_params[4];
static int cur_nparams;

static int param_index(const char *name) {
    int i; for (i = 0; i < cur_nparams && i < 3; i++) if (cur_params[i] && strcmp(cur_params[i], name) == 0) return i;
    return -1;
}

static void emit_load_imm(int64_t v) {
    if (v >= 0 && v <= 255) { movs_imm(0, (int)v); return; }
    if (nlit >= MAX_LIT) { die("too many literals"); return; }
    lits[nlit].ldr = (int)ctlen; lits[nlit].kind = 0; lits[nlit].val = (uint32_t)v; nlit++;
    e16(0x4800); /* ldr r0, [pc, #imm] patched later */
}

static void emit_string(const char *s, int len) {
    uint32_t off;
    if (crolen + (uint32_t)len + 1 > sizeof cro) { die("rodata full"); return; }
    off = crolen;
    memcpy(cro + crolen, s, (size_t)len);
    cro[crolen + len] = 0;
    crolen += (uint32_t)len + 1;
    if (nlit >= MAX_LIT) { die("too many literals"); return; }
    lits[nlit].ldr = (int)ctlen; lits[nlit].kind = 1; lits[nlit].val = off; nlit++;
    e16(0x4800);
}

static void flush_lits(void) {
    int i;
    if (ctlen & 2) e16(0xBF00);
    for (i = lit0; i < nlit; i++) {
        uint32_t lit_at = ctlen;
        uint32_t pc, imm;
        uint32_t word = lits[i].val;
        if (ctlen + 4 > sizeof ctext) { die("text full"); return; }
        ctext[ctlen++] = (uint8_t)word;
        ctext[ctlen++] = (uint8_t)(word >> 8);
        ctext[ctlen++] = (uint8_t)(word >> 16);
        ctext[ctlen++] = (uint8_t)(word >> 24);
        if (lits[i].kind == 1) {
            if (ncrel >= MAX_CREL) { die("too many relocs"); return; }
            crels[ncrel].off = lit_at; crels[ncrel].kind = REL_RO; crels[ncrel].sym = 0; ncrel++;
        }
        pc = (uint32_t)(lits[i].ldr + 4) & ~3u;
        if (lit_at < pc || ((lit_at - pc) & 3) || ((lit_at - pc) >> 2) > 255) { die("literal pool out of range"); return; }
        imm = (lit_at - pc) >> 2;
        ctext[lits[i].ldr] = (uint8_t)imm;
        ctext[lits[i].ldr + 1] = (uint8_t)(0x48 | (0 << 3)); /* rt = r0 */
        (void)imm;
    }
    lit0 = nlit;
}

static void expr_or(void);

static void expr_primary(void) {
    if (Tkind() == TK_NUM) { emit_load_imm(toks[ti].num); bump(); return; }
    if (Tkind() == TK_STR) {
        /* adjacent strings */
        char buf[512]; int n = 0;
        while (Tkind() == TK_STR && !failed) {
            if (n + toks[ti].len > (int)sizeof buf) { die("string too long"); return; }
            memcpy(buf + n, toks[ti].str, (size_t)toks[ti].len);
            n += toks[ti].len; bump();
        }
        emit_string(buf, n);
        return;
    }
    if (Tkind() == TK_IDENT) {
        const char *name = toks[ti].str;
        int pi;
        bump();
        if (eat_p('(')) {
            int argc = 0;
            sub_sp(16);
            if (!see_p(')')) {
                do {
                    if (argc >= 4) { die("more than 4 call arguments"); return; }
                    expr_or();
                    str_sp(0, argc * 4);
                    argc++;
                } while (eat_p(','));
            }
            expect_p(')');
            for (pi = 0; pi < argc; pi++) ldr_sp(pi, pi * 4);
            emit_bl(name);
            add_sp(16);
            return;
        }
        pi = param_index(name);
        if (pi < 0) { die("unknown identifier '%s'", name); return; }
        mov_rr(0, 4 + pi);
        return;
    }
    if (eat_p('(')) { expr_or(); expect_p(')'); return; }
    die("expected expression (line %d)", ti < ntok ? toks[ti].line : 0);
}

static int looks_like_cast(void) {
    int save, yes;
    if (!see_p('(')) return 0;
    save = ti; bump(); yes = is_type_start(); ti = save; return yes;
}

static void expr_unary(void) {
    if (eat_p('!')) { expr_unary(); e16(0x2800); emit_setcc(0); return; }
    if (eat_p('~')) { expr_unary(); e16(0x43C0); return; }          /* mvns r0, r0 */
    if (eat_p('-') || see_p(PU_DEC)) {
        if (see_p(PU_DEC)) { die("decrement is not supported"); return; }
        bump(); expr_unary(); e16(0x4240); return;                   /* rsbs r0, r0, #0 */
    }
    if (eat_p('+')) { expr_unary(); return; }
    if (eat_p('*')) { die("pointer dereference is not supported"); return; }
    if (eat_p('&')) { die("address-of is not supported"); return; }
    if (see_p(PU_INC)) { die("increment is not supported"); return; }
    if (see_id("sizeof")) { die("sizeof is not supported"); return; }
    if (looks_like_cast()) {
        int td = 0, st = 0;
        bump();
        parse_decl_specs(&td, &st);
        if (!see_p(')')) { Decl d; memset(&d, 0, sizeof d); parse_declarator(&d); }
        expect_p(')');
        expr_unary();
        return;
    }
    expr_primary();
    if (see_p(PU_ARROW) || see_p('.') || see_p('[')) { die("struct and array access are not supported"); return; }
}

static void bin_save(void) { sub_sp(8); str_sp(0, 0); }
static void bin_restore(void) { ldr_sp(1, 0); add_sp(8); } /* r1 = lhs, r0 = rhs */

static void expr_mul(void) {
    expr_unary();
    while (!failed && (see_p('*') || see_p('/') || see_p('%'))) {
        int op = Tp(); bump();
        bin_save(); expr_unary(); bin_restore();
        if (op == '*') { mov_rr(3, 1); e16(0x4343); mov_rr(0, 3); }          /* muls r3, r0; r3 was lhs */
        else if (op == '/') { e16(0xFB91); e16(0xF0F0); }                    /* sdiv r0, r1, r0 */
        else {
            e16(0xFB91); e16(0xF2F0);                                        /* sdiv r2, r1, r0 */
            mov_rr(3, 2); e16(0x4343);                                       /* muls r3, r0 */
            e16(0x1AC8);                                                     /* subs r0, r1, r3 */
        }
    }
}
static void expr_add(void) {
    expr_mul();
    while (!failed && (see_p('+') || see_p('-'))) {
        int op = Tp(); bump();
        bin_save(); expr_mul(); bin_restore();
        if (op == '+') e16(0x1808); else e16(0x1A08); /* adds/subs r0, r1, r0 */
    }
}
static void expr_shift(void) {
    expr_add();
    while (!failed && (see_p(PU_SHL) || see_p(PU_SHR))) {
        int op = Tp(); bump();
        bin_save(); expr_add(); bin_restore();
        /* lhs in r1, rhs in r0: LSLS/ASRS r1, r0 then mov r0, r1 */
        e16(op == PU_SHL ? 0x4081 : 0x4101);
        mov_rr(0, 1);
    }
}
static void expr_rel(void) {
    expr_shift();
    while (!failed && (see_p('<') || see_p('>') || see_p(PU_LE) || see_p(PU_GE))) {
        int op = Tp(); int cond;
        bump();
        bin_save(); expr_shift(); bin_restore();
        e16(0x4281); /* cmp r1, r0 */
        cond = op == '<' ? 11 : op == '>' ? 12 : op == PU_LE ? 13 : 10;
        emit_setcc(cond);
    }
}
static void expr_eq(void) {
    expr_rel();
    while (!failed && (see_p(PU_EQ) || see_p(PU_NE))) {
        int op = Tp(); bump();
        bin_save(); expr_rel(); bin_restore();
        e16(0x4281);
        emit_setcc(op == PU_EQ ? 0 : 1);
    }
}
static void expr_band(void) {
    expr_eq();
    while (eat_p('&')) { bin_save(); expr_eq(); bin_restore(); e16(0x4001); mov_rr(0, 1); }
}
static void expr_bxor(void) {
    expr_band();
    while (eat_p('^')) { bin_save(); expr_band(); bin_restore(); e16(0x4041); mov_rr(0, 1); }
}
static void expr_bor(void) {
    expr_bxor();
    while (eat_p('|')) { bin_save(); expr_bxor(); bin_restore(); e16(0x4301); mov_rr(0, 1); }
}
static void expr_land(void) {
    expr_bor();
    while (see_p(PU_LAND)) {
        int lf = new_lab(), le = new_lab();
        bump();
        e16(0x2800); emit_b(0, lf);
        expr_bor();
        e16(0x2800); emit_b(0, lf);
        movs_imm(0, 1); emit_b(14, le);
        bind_lab(lf); movs_imm(0, 0); bind_lab(le);
    }
}
static void expr_or(void) {
    expr_land();
    while (see_p(PU_LOR)) {
        int lt = new_lab(), le = new_lab();
        bump();
        e16(0x2800); emit_b(1, lt); /* bne */
        expr_land();
        e16(0x2800); emit_b(1, lt);
        movs_imm(0, 0); emit_b(14, le);
        bind_lab(lt); movs_imm(0, 1); bind_lab(le);
    }
    if (see_p('?')) { die("ternary is not supported"); return; }
    if (see_p('=')) { die("assignment is not supported"); return; }
}

static void stmt(void) {
    if (failed) return;
    if (eat_p('{')) {
        while (!see_p('}') && Tkind() != TK_EOF && !failed) stmt();
        expect_p('}');
        return;
    }
    if (eat_id("if")) {
        int lelse = new_lab(), lend = new_lab();
        expect_p('('); expr_or(); expect_p(')');
        e16(0x2800); emit_b(0, lelse);
        stmt();
        if (eat_id("else")) { emit_b(14, lend); bind_lab(lelse); stmt(); bind_lab(lend); }
        else bind_lab(lelse);
        return;
    }
    if (see_id("while") || see_id("for") || see_id("do") || see_id("switch")) {
        die("loops and switch are not supported"); return;
    }
    if (eat_id("return")) {
        if (!see_p(';')) expr_or();
        else movs_imm(0, 0);
        emit_b(14, epi_lab);
        expect_p(';');
        return;
    }
    if (eat_p(';')) return;
    if (is_type_start()) { die("local variables are not supported"); return; }
    expr_or();
    expect_p(';');
}

static void compile_fn(Decl *d) {
    int i;
    uint32_t start;
    if (d->nparams > 3) { die("more than 3 parameters are not supported"); return; }
    if (d->ellipsis) { die("defining a varargs function is not supported"); return; }
    while (ctlen & 3) e16(0xBF00);
    start = ctlen;
    cur_nparams = d->nparams;
    for (i = 0; i < 4; i++) cur_params[i] = i < d->nparams ? d->params[i] : NULL;
    nbr = 0; nlab = 0; epi_lab = new_lab();
    e16(0xB570);                         /* push {r4,r5,r6,lr} */
    mov_rr(4, 0); mov_rr(5, 1); mov_rr(6, 2);
    expect_p('{');
    while (!see_p('}') && Tkind() != TK_EOF && !failed) stmt();
    expect_p('}');
    movs_imm(0, 0);                      /* fall off the end -> 0 */
    bind_lab(epi_lab);
    e16(0xBD70);                         /* pop {r4,r5,r6,pc} */
    patch_brs();
    flush_lits();
    {
        int sym = csym_find(d->name);
        if (sym < 0) {
            if (ncsym >= MAX_CSYM) { die("too many symbols"); return; }
            sym = ncsym++;
        }
        csyms[sym].name = d->name;
        csyms[sym].value = start | 1u;
        csyms[sym].size = ctlen - start;
        csyms[sym].defined = 1;
        csyms[sym].bind = d->is_static ? 2 : 1;
    }
}

static void parse_file(void) {
    while (Tkind() != TK_EOF && !failed) {
        int td = 0, st = 0;
        Decl d;
        if (eat_p(';')) continue;
        memset(&d, 0, sizeof d);
        parse_decl_specs(&td, &st);
        d.is_static = st;
        if (eat_p(';')) continue;
        parse_declarator(&d);
        if (see_p('{')) {
            if (td) { die("typedef has a body"); return; }
            if (!d.is_func || !d.name) { die("expected a function"); return; }
            compile_fn(&d);
            continue;
        }
        if (td && d.name) add_typedef(d.name);
        if (see_p('=')) skip_initializer();
        while (eat_p(',')) {
            Decl e; memset(&e, 0, sizeof e);
            parse_declarator(&e);
            if (td && e.name) add_typedef(e.name);
            if (see_p('=')) skip_initializer();
        }
        expect_p(';');
    }
}

/* ---------------------------------------------------------------- ELF object */

static uint8_t objmem[96 * 1024];
static uint32_t objlen;

static void wr16(uint8_t *p, uint32_t o, uint16_t v) { p[o] = (uint8_t)v; p[o + 1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t o, uint32_t v) {
    p[o] = (uint8_t)v; p[o + 1] = (uint8_t)(v >> 8); p[o + 2] = (uint8_t)(v >> 16); p[o + 3] = (uint8_t)(v >> 24);
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static uint32_t str_put(uint8_t *tab, uint32_t *len, uint32_t cap, const char *s) {
    uint32_t off = *len, n = (uint32_t)strlen(s) + 1;
    if (off + n > cap) { die("string table full"); return 0; }
    memcpy(tab + off, s, n); *len += n; return off;
}

static int write_object(void) {
    uint8_t strs[2048], shstr[256];
    uint32_t slen = 1, shlen = 1;
    uint32_t off, shoff;
    int nsec, si_text, si_ro, si_sym, si_str, si_sh, si_rel;
    int nsym, i, rels = 0;
    int sym_of[MAX_CSYM];
    memset(strs, 0, sizeof strs); memset(shstr, 0, sizeof shstr); memset(objmem, 0, sizeof objmem);
    for (i = 0; i < ncrel; i++) if (crels[i].kind == REL_CALL || crels[i].kind == REL_RO) rels++;
    nsec = 1; /* null */
    si_text = nsec++;
    si_ro = crolen ? nsec++ : -1;
    si_rel = rels ? nsec++ : -1;
    si_sym = nsec++;
    si_str = nsec++;
    si_sh = nsec++;
    /* symbols: null, text sec, ro sec?, functions, undefs */
    nsym = 1;
    if (si_ro >= 0) nsym++; /* we'll number explicitly below */
    {
        uint8_t *sym = objmem; /* filled after we know offsets; build in a side buffer */
        (void)sym;
    }
    /* Layout: ehdr, text, ro, rel, sym, str, shstr, shdrs. Build content first. */
    off = 52;
    while (off & 3) off++;
    if (off + ctlen + crolen + 16 > sizeof objmem) { die("object too large"); return 0; }
    memcpy(objmem + off, ctext, ctlen);
    {
        uint32_t text_off = off; off += ctlen;
        uint32_t ro_off = 0;
        if (crolen) {
            while (off & 3) off++;
            ro_off = off; memcpy(objmem + off, cro, crolen); off += crolen;
        }
        /* symbol table built at `off` later; first assign string indexes */
        strs[0] = 0; slen = 1;
        for (i = 0; i < ncsym; i++) sym_of[i] = -1;
        {
            uint8_t symtab[64 * 16];
            int ns = 0;
            int ro_sym = -1, text_sym;
            memset(symtab, 0, sizeof symtab);
            ns = 1; /* null */
            text_sym = ns++;
            wr32(symtab, text_sym * 16 + 0, 0);
            wr32(symtab, text_sym * 16 + 4, 0);
            wr32(symtab, text_sym * 16 + 8, 0);
            symtab[text_sym * 16 + 12] = 3; /* STT_SECTION */
            wr16(symtab, text_sym * 16 + 14, (uint16_t)si_text);
            if (si_ro >= 0) {
                ro_sym = ns++;
                symtab[ro_sym * 16 + 12] = 3;
                wr16(symtab, ro_sym * 16 + 14, (uint16_t)si_ro);
            }
            for (i = 0; i < ncsym; i++) {
                if (!csyms[i].defined) continue;
                sym_of[i] = ns;
                wr32(symtab, ns * 16, str_put(strs, &slen, sizeof strs, csyms[i].name));
                wr32(symtab, ns * 16 + 4, csyms[i].value);
                wr32(symtab, ns * 16 + 8, csyms[i].size);
                symtab[ns * 16 + 12] = (uint8_t)((csyms[i].bind == 2 ? 0 : 1) << 4) | 2; /* FUNC */
                wr16(symtab, ns * 16 + 14, (uint16_t)si_text);
                ns++;
            }
            for (i = 0; i < ncsym; i++) {
                if (csyms[i].defined) continue;
                sym_of[i] = ns;
                wr32(symtab, ns * 16, str_put(strs, &slen, sizeof strs, csyms[i].name));
                symtab[ns * 16 + 12] = (uint8_t)(1 << 4); /* GLOBAL NOTYPE */
                wr16(symtab, ns * 16 + 14, 0);
                ns++;
            }
            {
                uint8_t rel[MAX_CREL * 8];
                int nr = 0;
                uint32_t rel_off = 0, sym_off, str_off, shs_off;
                memset(rel, 0, sizeof rel);
                for (i = 0; i < ncrel; i++) {
                    uint32_t info;
                    int typ = crels[i].kind == REL_CALL ? 10 : 2;
                    int sy = crels[i].kind == REL_RO ? ro_sym : sym_of[crels[i].sym];
                    if (sy < 0) { die("bad reloc symbol"); return 0; }
                    info = ((uint32_t)sy << 8) | (uint32_t)typ;
                    wr32(rel, nr * 8, crels[i].off);
                    wr32(rel, nr * 8 + 4, info);
                    nr++;
                }
                if (si_rel >= 0) {
                    while (off & 3) off++;
                    rel_off = off;
                    if (off + (uint32_t)nr * 8 > sizeof objmem) { die("object too large"); return 0; }
                    memcpy(objmem + off, rel, (size_t)nr * 8); off += (uint32_t)nr * 8;
                }
                while (off & 3) off++;
                sym_off = off;
                if (off + (uint32_t)ns * 16 > sizeof objmem) { die("object too large"); return 0; }
                memcpy(objmem + off, symtab, (size_t)ns * 16); off += (uint32_t)ns * 16;
                str_off = off;
                memcpy(objmem + off, strs, slen); off += slen;
                shstr[0] = 0; shlen = 1;
                {
                    uint32_t nm_text = str_put(shstr, &shlen, sizeof shstr, ".text");
                    uint32_t nm_ro = crolen ? str_put(shstr, &shlen, sizeof shstr, ".rodata") : 0;
                    uint32_t nm_rel = rels ? str_put(shstr, &shlen, sizeof shstr, ".rel.text") : 0;
                    uint32_t nm_sym = str_put(shstr, &shlen, sizeof shstr, ".symtab");
                    uint32_t nm_str = str_put(shstr, &shlen, sizeof shstr, ".strtab");
                    uint32_t nm_sh = str_put(shstr, &shlen, sizeof shstr, ".shstrtab");
                    int first_global = 1 + (si_ro >= 0 ? 1 : 0);
                    /* locals are null + section syms; our defined funcs may be local.
                       sh_info = index of first global. Count leading locals. */
                    int fg = 1;
                    for (i = 1; i < ns; i++) {
                        int bind = symtab[i * 16 + 12] >> 4;
                        if (bind == 0) fg = i + 1; else break;
                    }
                    first_global = fg;
                    shs_off = off;
                    memcpy(objmem + off, shstr, shlen); off += shlen;
                    while (off & 3) off++;
                    shoff = off;
                    if (shoff + (uint32_t)nsec * 40 > sizeof objmem) { die("object too large"); return 0; }
                    /* write headers in index order */
                    {
                        int idx = 0;
                        /* 0 null already zero */
                        idx = 1;
                        wr32(objmem, shoff + idx * 40, nm_text);
                        wr32(objmem, shoff + idx * 40 + 4, 1);
                        wr32(objmem, shoff + idx * 40 + 8, 6); /* AX */
                        wr32(objmem, shoff + idx * 40 + 16, text_off);
                        wr32(objmem, shoff + idx * 40 + 20, ctlen);
                        wr32(objmem, shoff + idx * 40 + 32, 4);
                        idx++;
                        if (si_ro >= 0) {
                            wr32(objmem, shoff + idx * 40, nm_ro);
                            wr32(objmem, shoff + idx * 40 + 4, 1);
                            wr32(objmem, shoff + idx * 40 + 8, 2);
                            wr32(objmem, shoff + idx * 40 + 16, ro_off);
                            wr32(objmem, shoff + idx * 40 + 20, crolen);
                            wr32(objmem, shoff + idx * 40 + 32, 4);
                            idx++;
                        }
                        if (si_rel >= 0) {
                            wr32(objmem, shoff + idx * 40, nm_rel);
                            wr32(objmem, shoff + idx * 40 + 4, 9);
                            wr32(objmem, shoff + idx * 40 + 16, rel_off);
                            wr32(objmem, shoff + idx * 40 + 20, (uint32_t)nr * 8);
                            wr32(objmem, shoff + idx * 40 + 24, (uint32_t)si_sym);
                            wr32(objmem, shoff + idx * 40 + 28, (uint32_t)si_text);
                            wr32(objmem, shoff + idx * 40 + 32, 4);
                            wr32(objmem, shoff + idx * 40 + 36, 8);
                            idx++;
                        }
                        wr32(objmem, shoff + idx * 40, nm_sym);
                        wr32(objmem, shoff + idx * 40 + 4, 2);
                        wr32(objmem, shoff + idx * 40 + 16, sym_off);
                        wr32(objmem, shoff + idx * 40 + 20, (uint32_t)ns * 16);
                        wr32(objmem, shoff + idx * 40 + 24, (uint32_t)si_str);
                        wr32(objmem, shoff + idx * 40 + 28, (uint32_t)first_global);
                        wr32(objmem, shoff + idx * 40 + 32, 4);
                        wr32(objmem, shoff + idx * 40 + 36, 16);
                        idx++;
                        wr32(objmem, shoff + idx * 40, nm_str);
                        wr32(objmem, shoff + idx * 40 + 4, 3);
                        wr32(objmem, shoff + idx * 40 + 16, str_off);
                        wr32(objmem, shoff + idx * 40 + 20, slen);
                        idx++;
                        wr32(objmem, shoff + idx * 40, nm_sh);
                        wr32(objmem, shoff + idx * 40 + 4, 3);
                        wr32(objmem, shoff + idx * 40 + 16, shs_off);
                        wr32(objmem, shoff + idx * 40 + 20, shlen);
                        (void)idx;
                    }
                    objmem[0] = 0x7f; objmem[1] = 'E'; objmem[2] = 'L'; objmem[3] = 'F';
                    objmem[4] = 1; objmem[5] = 1; objmem[6] = 1;
                    wr16(objmem, 16, 1);             /* ET_REL */
                    wr16(objmem, 18, 40);            /* EM_ARM */
                    wr32(objmem, 20, 1);
                    wr32(objmem, 32, shoff);
                    wr32(objmem, 36, 0x05000000);
                    wr16(objmem, 40, 52);
                    wr16(objmem, 46, 40);
                    wr16(objmem, 48, (uint16_t)nsec);
                    wr16(objmem, 50, (uint16_t)si_sh);
                    objlen = shoff + (uint32_t)nsec * 40;
                    (void)first_global;
                    return 1;
                }
            }
        }
    }
}

/* ---------------------------------------------------------------- linker */

enum { MAX_SEC = 2048, MAX_SYM = 8192, MAX_REL = 8192, MAX_AR = 4096 };
typedef struct {
    const char *name;
    const uint8_t *data;
    uint32_t size, flags, type, align, out_addr, out_off;
    uint8_t *out_buf;
    int live;
} Sec;
typedef struct {
    const char *name;
    uint32_t value, size;
    uint8_t info, other;
    uint16_t shndx;
    int sec;
} Sym;
typedef struct { uint32_t offset, type; int sym, def, target; } Rel;
typedef struct { const char *name; uint32_t off; int file; int loaded; } ArEnt;

static Sec lsecs[MAX_SEC];
static Sym lsyms[MAX_SYM];
static Rel lrels[MAX_REL];
static ArEnt ars[MAX_AR];
static int nsec, nsym, nrel, nar;
static const char *empty_str = "";

static uint32_t align_up(uint32_t v, uint32_t a) {
    if (a < 2) return v;
    return (v + a - 1u) & ~(a - 1u);
}

static int add_elf(const uint8_t *d, uint32_t n) {
    uint32_t shoff, shstr_off;
    uint16_t shnum, shstrndx, shentsize;
    int i, sym_base, sec_base;
    int map[1024];
    const uint8_t *shstr;
    if (n < 52 || d[0] != 0x7f || d[1] != 'E' || d[2] != 'L' || d[3] != 'F' || d[4] != 1 || d[5] != 1) {
        die("not an ELF32 LE file"); return 0;
    }
    if (rd16(d + 18) != 40) { die("not an ARM ELF"); return 0; }
    shoff = rd32(d + 32);
    shentsize = rd16(d + 46);
    shnum = rd16(d + 48);
    shstrndx = rd16(d + 50);
    if (!shentsize) shentsize = 40;
    if (shnum > 1024) { die("too many sections in one object"); return 0; }
    if (shoff + (uint32_t)shnum * shentsize > n) { die("truncated ELF"); return 0; }
    sec_base = nsec;
    shstr_off = rd32(d + shoff + shstrndx * shentsize + 16);
    shstr = d + shstr_off;
    for (i = 0; i < shnum; i++) {
        const uint8_t *sh = d + shoff + (uint32_t)i * shentsize;
        uint32_t name = rd32(sh), type = rd32(sh + 4), flags = rd32(sh + 8);
        uint32_t offset = rd32(sh + 16), size = rd32(sh + 20), align = rd32(sh + 32);
        Sec *s;
        if (nsec >= MAX_SEC) { die("too many sections"); return 0; }
        map[i] = nsec;
        s = &lsecs[nsec++];
        memset(s, 0, sizeof *s);
        s->name = (name && shstr_off + name < n) ? (const char *)(shstr + name) : empty_str;
        s->type = type; s->flags = flags; s->size = size;
        s->align = align ? align : 1;
        if (type != 8 && size && offset + size <= n) s->data = d + offset; /* not NOBITS */
        if (type == 4) { die("RELA relocations are not supported"); return 0; }
    }
    sym_base = nsym;
    for (i = 0; i < shnum && !failed; i++) {
        Sec *s = &lsecs[map[i]];
        if (s->type != 2) continue; /* SYMTAB */
        {
            const uint8_t *sh = d + shoff + (uint32_t)i * shentsize;
            uint32_t link = rd32(sh + 24);
            uint32_t entsize = rd32(sh + 36); if (!entsize) entsize = 16;
            uint32_t count = s->size / entsize;
            const uint8_t *strtab = NULL;
            uint32_t k;
            if (link < shnum) {
                uint32_t soff = rd32(d + shoff + link * shentsize + 16);
                if (soff < n) strtab = d + soff;
            }
            for (k = 0; k < count; k++) {
                const uint8_t *ent = s->data + k * entsize;
                Sym *sy;
                uint32_t nm;
                uint16_t shndx;
                if (!s->data) break;
                if (nsym >= MAX_SYM) { die("too many symbols"); return 0; }
                sy = &lsyms[nsym++];
                memset(sy, 0, sizeof *sy);
                nm = rd32(ent);
                sy->value = rd32(ent + 4);
                sy->size = rd32(ent + 8);
                sy->info = ent[12];
                sy->other = ent[13];
                shndx = rd16(ent + 14);
                sy->shndx = shndx;
                sy->name = (strtab && nm) ? (const char *)(strtab + nm) : empty_str;
                sy->sec = (shndx > 0 && shndx < shnum) ? map[shndx] : -1;
            }
            break;
        }
    }
    for (i = 0; i < shnum && !failed; i++) {
        Sec *s = &lsecs[map[i]];
        if (s->type != 9 || !s->data) continue;
        {
            const uint8_t *sh = d + shoff + (uint32_t)i * shentsize;
            uint32_t info = rd32(sh + 28);
            uint32_t count = s->size / 8;
            uint32_t k;
            int target = (info < shnum) ? map[info] : -1;
            for (k = 0; k < count; k++) {
                Rel *r; uint32_t info_r;
                if (nrel >= MAX_REL) { die("too many relocs"); return 0; }
                r = &lrels[nrel++];
                r->offset = rd32(s->data + k * 8);
                info_r = rd32(s->data + k * 8 + 4);
                r->type = info_r & 0xff;
                r->sym = sym_base + (int)(info_r >> 8);
                r->def = -1;
                r->target = target;
            }
        }
    }
    (void)sec_base; (void)sym_base;
    return !failed;
}

static int find_defined(const char *name) {
    int i, weak = -1;
    if (!name || !name[0]) return -1;
    for (i = 0; i < nsym; i++) {
        int bind;
        if (lsyms[i].shndx == 0 || lsyms[i].shndx >= 0xFF00) continue;
        if (!lsyms[i].name || strcmp(lsyms[i].name, name) != 0) continue;
        bind = lsyms[i].info >> 4;
        if (bind == 1) return i;
        if (bind == 2) weak = i;
    }
    return weak;
}

static int member_loaded[64][8];
static uint32_t member_off[64][8];
static int member_n[64];

static int pull_member(int file, uint32_t off) {
    const uint8_t *d; uint32_t sz, i;
    int mi;
    if (file < 0 || file >= nfiles) return 0;
    for (mi = 0; mi < member_n[file]; mi++) if (member_off[file][mi] == off) return 1;
    d = files[file].data; sz = files[file].size;
    if (off + 60 > sz) { die("bad archive offset"); return 0; }
    {
        uint32_t msz = 0; const uint8_t *p = d + off + 48;
        for (i = 0; i < 10 && p[i] >= '0' && p[i] <= '9'; i++) msz = msz * 10 + (uint32_t)(p[i] - '0');
        if (off + 60 + msz > sz) { die("truncated archive member"); return 0; }
        if (member_n[file] < 8) { member_off[file][member_n[file]++] = off; }
        return add_elf(d + off + 60, msz);
    }
}

static int resolve_name(const char *name) {
    int d = find_defined(name);
    int i;
    if (d >= 0) return d;
    for (i = 0; i < nar; i++) {
        if (strcmp(ars[i].name, name) != 0) continue;
        if (!pull_member(ars[i].file, ars[i].off)) return -1;
        d = find_defined(name);
        if (d >= 0) return d;
    }
    return -1;
}

static void mark(int si);
static void resolve_rel(int ri) {
    Rel *r = &lrels[ri];
    Sym *s;
    int def;
    if (r->sym < 0 || r->sym >= nsym) { die("reloc symbol out of range"); return; }
    s = &lsyms[r->sym];
    if (s->shndx == 0) {
        def = resolve_name(s->name);
        if (def < 0) {
            if ((s->info >> 4) == 2) { r->def = r->sym; return; } /* weak */
            die("undefined symbol '%s'", s->name ? s->name : "?");
            return;
        }
        r->def = def;
        if (lsyms[def].sec >= 0) mark(lsyms[def].sec);
    } else {
        r->def = r->sym;
        if (s->sec >= 0) mark(s->sec);
    }
}
static void mark(int si) {
    int i;
    if (si < 0 || si >= nsec) return;
    if (lsecs[si].live) return;
    if (!(lsecs[si].flags & 2)) return;
    lsecs[si].live = 1;
    if (!lsecs[si].size) return;
    for (i = 0; i < nrel; i++) if (lrels[i].target == si) resolve_rel(i);
}

static int load_archive(int file) {
    const uint8_t *d = files[file].data;
    uint32_t n = files[file].size;
    uint32_t off = 8;
    const uint8_t *strtab = NULL; uint32_t strsz = 0;
    if (n < 8 || memcmp(d, "!<arch>\n", 8) != 0) { die("not a GNU archive"); return 0; }
    while (off + 60 <= n && !failed) {
        char name[64]; uint32_t msz = 0, i;
        const uint8_t *h = d + off;
        memset(name, 0, sizeof name);
        memcpy(name, h, 16);
        for (i = 16; i > 0 && (name[i - 1] == ' ' || name[i - 1] == '/'); i--) name[i - 1] = 0;
        for (i = 0; i < 10 && h[48 + i] >= '0' && h[48 + i] <= '9'; i++) msz = msz * 10 + (uint32_t)(h[48 + i] - '0');
        if (strcmp(name, "/") == 0) {
            uint32_t ns, k; const uint8_t *b = d + off + 60;
            if (msz < 4) { die("bad ar index"); return 0; }
            ns = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
            if (4 + ns * 4 > msz) { die("bad ar index"); return 0; }
            {
                const uint8_t *np = b + 4 + ns * 4;
                for (k = 0; k < ns; k++) {
                    uint32_t so = ((uint32_t)b[4 + k * 4] << 24) | ((uint32_t)b[5 + k * 4] << 16) |
                                  ((uint32_t)b[6 + k * 4] << 8) | b[7 + k * 4];
                    const uint8_t *ne = np; uint32_t nl;
                    while (*ne) ne++;
                    nl = (uint32_t)(ne - np);
                    if (nar >= MAX_AR) { die("too many archive symbols"); return 0; }
                    ars[nar].name = adup((const char *)np, nl);
                    ars[nar].off = so; ars[nar].file = file; ars[nar].loaded = 0;
                    nar++;
                    np = ne + 1;
                }
            }
        } else if (strcmp(name, "//") == 0) {
            strtab = d + off + 60; strsz = msz; (void)strsz;
        }
        off += 60 + msz;
        if (off & 1) off++;
        (void)strtab;
    }
    return !failed;
}

static int cat_of(const Sec *s) {
    const char *n = s->name ? s->name : "";
    if (!(s->flags & 2)) return 0;
    if (strcmp(n, ".text") == 0 || strncmp(n, ".text.", 6) == 0 || strcmp(n, ".glue_7") == 0 || strcmp(n, ".glue_7t") == 0) return 1;
    if (strcmp(n, ".rodata") == 0 || strncmp(n, ".rodata.", 8) == 0) return 2;
    if (strncmp(n, ".pex.got", 8) == 0) return 3;
    if (strcmp(n, ".data") == 0 || strncmp(n, ".data.", 6) == 0) return 4;
    if (strcmp(n, ".bss") == 0 || strncmp(n, ".bss.", 5) == 0) return 5;
    if (strcmp(n, ".pex.exports") == 0) return 6;
    if (strcmp(n, ".pex.weak") == 0) return 7;
    if (strcmp(n, ".pex.meta") == 0) return 8;
    if (strcmp(n, ".init_array") == 0 || strncmp(n, ".init_array.", 12) == 0 || strcmp(n, ".pex.init") == 0) return 9;
    if (strcmp(n, ".fini_array") == 0 || strncmp(n, ".fini_array.", 12) == 0 || strcmp(n, ".pex.fini") == 0) return 10;
    if (strncmp(n, ".ARM.exidx", 10) == 0 || strncmp(n, ".ARM.extab", 10) == 0 || strncmp(n, ".eh_frame", 9) == 0 ||
        strncmp(n, ".comment", 8) == 0 || strncmp(n, ".note", 5) == 0) return 0;
    return -1;
}

static uint8_t otext[128 * 1024], orodata[64 * 1024], odata[64 * 1024];
static uint8_t ometa[5][32 * 1024];
static uint32_t otlen, orlen, odlen, omlen[5];

static uint32_t place_list(int *ids, int n, uint8_t *buf, uint32_t *len, uint32_t cap, uint32_t addr, uint32_t end_align) {
    int i;
    for (i = 0; i < n; i++) {
        Sec *s = &lsecs[ids[i]];
        uint32_t al = s->align < 2 ? 1 : s->align;
        uint32_t np = align_up(addr, al) - addr;
        if (*len + np + s->size > cap) { die("linked section too large"); return addr; }
        if (np) memset(buf + *len, 0, np);
        *len += np; addr += np;
        s->out_addr = addr; s->out_buf = buf; s->out_off = *len;
        if (s->type == 8 || !s->data) memset(buf + *len, 0, s->size);
        else memcpy(buf + *len, s->data, s->size);
        *len += s->size; addr += s->size;
    }
    {
        uint32_t np = align_up(addr, end_align) - addr;
        if (*len + np > cap) { die("linked section too large"); return addr; }
        if (np) memset(buf + *len, 0, np);
        *len += np; addr += np;
    }
    return addr;
}

static int32_t dec_bl(const uint8_t *p) {
    uint32_t hw1 = rd16(p), hw2 = rd16(p + 2);
    uint32_t S = (hw1 >> 10) & 1, J1 = (hw2 >> 13) & 1, J2 = (hw2 >> 11) & 1;
    uint32_t I1 = (~(J1 ^ S)) & 1, I2 = (~(J2 ^ S)) & 1;
    uint32_t imm10 = hw1 & 0x3ff, imm11 = hw2 & 0x7ff;
    uint32_t u = (S << 24) | (I1 << 23) | (I2 << 22) | (imm10 << 12) | (imm11 << 1);
    if (u & (1u << 24)) return (int32_t)u - (1 << 25);
    return (int32_t)u;
}
static void enc_bl(uint8_t *p, int32_t imm, int is_bl) {
    uint32_t u = (uint32_t)imm;
    uint32_t S = (u >> 24) & 1, I1 = (u >> 23) & 1, I2 = (u >> 22) & 1;
    uint32_t imm10 = (u >> 12) & 0x3ff, imm11 = (u >> 1) & 0x7ff;
    uint32_t J1 = (~(I1 ^ S)) & 1, J2 = (~(I2 ^ S)) & 1;
    uint32_t hw1 = 0xF000u | (S << 10) | imm10;
    uint32_t hw2 = (is_bl ? 0xD000u : 0x9000u) | (J1 << 13) | (J2 << 11) | imm11;
    p[0] = (uint8_t)hw1; p[1] = (uint8_t)(hw1 >> 8);
    p[2] = (uint8_t)hw2; p[3] = (uint8_t)(hw2 >> 8);
}

static uint8_t elfout[512 * 1024];
static uint32_t elfout_len;

static int link_all(void) {
    int i, main_sec = -1;
    int texts[512], nt = 0, ros[256], nr = 0, gots[256], ng = 0, datas[128], nd = 0, bsss[128], nb = 0;
    int metas[5][32], nmeta[5];
    uint32_t text_end, ro_addr, ro_end, data_addr, data_end, bss_addr, rw_end;
    int out_of[MAX_SEC];
    memset(nmeta, 0, sizeof nmeta);
    memset(out_of, 0xff, sizeof out_of);
    otlen = orlen = odlen = 0;
    memset(omlen, 0, sizeof omlen);
    for (i = 0; i < nfiles; i++) member_n[i] = 0;
    nsec = nsym = nrel = nar = 0;
    for (i = 0; i < nfiles && !failed; i++) {
        if (files[i].kind == FK_O) add_elf(files[i].data, files[i].size);
        else if (files[i].kind == FK_A) load_archive(i);
    }
    if (failed) return 0;
    for (i = 0; i < nsec; i++) {
        const char *n = lsecs[i].name ? lsecs[i].name : "";
        if (strcmp(n, ".text.pex_main") == 0 && lsecs[i].size) mark(i);
        if (strcmp(n, ".pex.exports") == 0 || strcmp(n, ".pex.weak") == 0 || strcmp(n, ".pex.meta") == 0) mark(i);
        if (strcmp(n, ".init_array") == 0 || strncmp(n, ".init_array.", 12) == 0 || strcmp(n, ".pex.init") == 0) mark(i);
        if (strcmp(n, ".fini_array") == 0 || strncmp(n, ".fini_array.", 12) == 0 || strcmp(n, ".pex.fini") == 0) mark(i);
    }
    {
        int pm = find_defined("pex_main");
        if (pm < 0) { die("no pex_main"); return 0; }
        if (lsyms[pm].sec >= 0) mark(lsyms[pm].sec);
    }
    if (failed) return 0;
    for (i = 0; i < nsec; i++) {
        int c;
        if (!lsecs[i].live || !lsecs[i].size) continue;
        c = cat_of(&lsecs[i]);
        if (c == 0) continue;
        if (c < 0) { die("unexpected allocated section %s", lsecs[i].name); return 0; }
        if (c == 1) {
            if (strcmp(lsecs[i].name, ".text.pex_main") == 0) main_sec = i;
            else if (nt < 512) texts[nt++] = i;
        } else if (c == 2) { if (nr < 256) ros[nr++] = i; }
        else if (c == 3) { if (ng < 256) gots[ng++] = i; }
        else if (c == 4) { if (nd < 128) datas[nd++] = i; }
        else if (c == 5) { if (nb < 128) bsss[nb++] = i; }
        else if (c >= 6 && c <= 10) {
            int k = c - 6;
            if (nmeta[k] < 32) metas[k][nmeta[k]++] = i;
        }
    }
    {
        int ordered[513]; int no = 0;
        if (main_sec >= 0) ordered[no++] = main_sec;
        for (i = 0; i < nt; i++) ordered[no++] = texts[i];
        text_end = place_list(ordered, no, otext, &otlen, sizeof otext, 0, 4);
        for (i = 0; i < no; i++) out_of[ordered[i]] = 1;
    }
    ro_addr = align_up(text_end, 4);
    ro_end = place_list(ros, nr, orodata, &orlen, sizeof orodata, ro_addr, 4);
    for (i = 0; i < nr; i++) out_of[ros[i]] = 2;
    if (!nr) ro_end = ro_addr;
    data_addr = align_up(ro_end, 8);
    {
        int both[384]; int nboth = 0;
        for (i = 0; i < ng; i++) both[nboth++] = gots[i];
        for (i = 0; i < nd; i++) both[nboth++] = datas[i];
        data_end = place_list(both, nboth, odata, &odlen, sizeof odata, data_addr, 4);
        for (i = 0; i < nboth; i++) out_of[both[i]] = 3;
        if (!nboth) { data_end = data_addr; odlen = 0; }
    }
    bss_addr = align_up(data_end, 4);
    {
        uint32_t bss_end = bss_addr;
        for (i = 0; i < nb; i++) {
            Sec *s = &lsecs[bsss[i]];
            bss_end = align_up(bss_end, s->align < 2 ? 1 : s->align);
            s->out_addr = bss_end; s->out_buf = NULL; s->out_off = 0;
            bss_end += s->size;
            out_of[bsss[i]] = 4;
        }
        rw_end = align_up(bss_end, 4);
    }
    /* meta */
    for (i = 0; i < 5; i++) {
        uint32_t addr = 0x70000000u + (uint32_t)i * 0x1000u;
        if (nmeta[i]) {
            place_list(metas[i], nmeta[i], ometa[i], &omlen[i], sizeof ometa[i], addr, 4);
            {
                int k; for (k = 0; k < nmeta[i]; k++) out_of[metas[i][k]] = 5 + i;
            }
        }
    }
    if (failed) return 0;
    /* apply relocs */
    for (i = 0; i < nrel && !failed; i++) {
        Rel *r = &lrels[i];
        Sec *ts;
        Sym *ds;
        uint8_t *loc;
        uint32_t P, S, addend;
        if (r->target < 0 || !lsecs[r->target].live || !lsecs[r->target].size) continue;
        if (r->def < 0) continue;
        ts = &lsecs[r->target];
        if (!ts->out_buf) continue;
        ds = &lsyms[r->def];
        loc = ts->out_buf + ts->out_off + r->offset;
        P = ts->out_addr + r->offset;
        if (ds->shndx >= 0xFF00) S = ds->value;
        else if (ds->sec < 0 || !lsecs[ds->sec].live) { die("reloc to unplaced symbol '%s'", ds->name); return 0; }
        else S = lsecs[ds->sec].out_addr + ds->value;
        if (r->type == 10 || r->type == 30) {
            int32_t A = dec_bl(loc);
            int32_t imm = (int32_t)((S & ~1u) + (uint32_t)A - P);
            if (imm < -(1 << 24) || imm >= (1 << 24)) { die("BL out of range"); return 0; }
            enc_bl(loc, imm, r->type == 10);
        } else if (r->type == 2 || r->type == 38) {
            addend = rd32(loc);
            wr32(loc, 0, S + addend);
        } else if (r->type == 0 || r->type == 40) {
            /* ignore */
        } else { die("unsupported relocation type %u", r->type); return 0; }
    }
    if (failed) return 0;

    /* emit ET_EXEC */
    {
        uint8_t strs[4096], shstr[512];
        uint32_t slen = 1, shlen = 1;
        uint8_t symtab[1024 * 16];
        uint8_t relbuf[2048 * 8];
        int ns = 0, nro = 0;
        int sec_sym_text, sec_sym_ro = -1, sec_sym_data = -1, sec_sym_bss = -1;
        int si = 0;
        uint32_t off;
        int emit_ro = orlen > 0;
        int emit_data = odlen > 0;
        int emit_bss = (rw_end > data_end);
        const char *meta_name[5] = { ".pex.exports", ".pex.weak", ".pex.meta", ".pex.init", ".pex.fini" };
        int have_meta[5];
        int nsec_out;
        memset(strs, 0, sizeof strs);
        memset(shstr, 0, sizeof shstr);
        memset(symtab, 0, sizeof symtab);
        memset(have_meta, 0, sizeof have_meta);
        for (i = 0; i < 5; i++) have_meta[i] = omlen[i] > 0;
        ns = 1;
        sec_sym_text = ns++;
        symtab[sec_sym_text * 16 + 12] = 3;
        wr16(symtab, sec_sym_text * 16 + 14, 1);
        wr32(symtab, sec_sym_text * 16 + 4, 0);
        si = 1; /* .text is section 1 */
        if (emit_ro) {
            sec_sym_ro = ns++;
            symtab[sec_sym_ro * 16 + 12] = 3;
            wr32(symtab, sec_sym_ro * 16 + 4, ro_addr);
            /* shndx filled once we know index: ro is 2 if present */
            wr16(symtab, sec_sym_ro * 16 + 14, 2);
            si++;
        }
        if (emit_data) {
            sec_sym_data = ns++;
            symtab[sec_sym_data * 16 + 12] = 3;
            wr32(symtab, sec_sym_data * 16 + 4, data_addr);
            wr16(symtab, sec_sym_data * 16 + 14, (uint16_t)(emit_ro ? 3 : 2));
            si++;
        }
        if (emit_bss) {
            sec_sym_bss = ns++;
            symtab[sec_sym_bss * 16 + 12] = 3;
            wr32(symtab, sec_sym_bss * 16 + 4, data_end);
            wr16(symtab, sec_sym_bss * 16 + 14, (uint16_t)si);
            si++;
        }
        {
            int base_after_bss = si;
            int mi;
            for (mi = 0; mi < 5; mi++) if (have_meta[mi]) {
                int ss = ns++;
                symtab[ss * 16 + 12] = 3;
                wr32(symtab, ss * 16 + 4, 0x70000000u + (uint32_t)mi * 0x1000u);
                wr16(symtab, ss * 16 + 14, (uint16_t)base_after_bss);
                base_after_bss++;
                (void)ss;
            }
        }
        /* named symbols from live sections */
        for (i = 0; i < nsym; i++) {
            Sym *s = &lsyms[i];
            int bind = s->info >> 4;
            int typ = s->info & 15;
            int oi;
            if (typ == 3 || typ == 4) continue; /* section, file */
            if (!s->name || !s->name[0] || s->name[0] == '$') continue;
            if (s->shndx == 0 || s->shndx >= 0xFF00) continue;
            if (s->sec < 0 || !lsecs[s->sec].live) continue;
            oi = out_of[s->sec];
            if (oi < 0) continue;
            if (ns >= 1000) { die("too many output symbols"); return 0; }
            wr32(symtab, ns * 16, str_put(strs, &slen, sizeof strs, s->name));
            wr32(symtab, ns * 16 + 4, lsecs[s->sec].out_addr + s->value);
            wr32(symtab, ns * 16 + 8, s->size);
            symtab[ns * 16 + 12] = s->info;
            symtab[ns * 16 + 13] = s->other;
            {
                uint16_t sh = 1;
                if (oi == 2) sh = 2;
                else if (oi == 3) sh = (uint16_t)(emit_ro ? 3 : 2);
                else if (oi == 4) sh = (uint16_t)(1 + emit_ro + emit_data);
                else if (oi >= 5) {
                    int k, idx = 1 + emit_ro + emit_data + emit_bss;
                    for (k = 0; k < oi - 5; k++) if (have_meta[k]) idx++;
                    sh = (uint16_t)idx;
                }
                wr16(symtab, ns * 16 + 14, sh);
            }
            /* remember this output index in sym.size's high? use a parallel via value stash: store in ArEnt? 
               Use the Sym.size field's companion: overload by keeping an array. */
            s->size = s->size; /* keep */
            /* stash output index in the unused 'other' copy? other is needed.
               We'll re-find by name when emitting relocs if needed. Store index in a side array. */
            ns++;
            (void)bind;
        }
        /* synthesized linker symbols */
        {
            int sh_data = emit_data ? (emit_ro ? 3 : 2) : 0;
            int sh_end = emit_bss ? (1 + emit_ro + emit_data) : 0xFFF1;
            wr32(symtab, ns * 16, str_put(strs, &slen, sizeof strs, "__pex_rw_start"));
            wr32(symtab, ns * 16 + 4, data_addr);
            symtab[ns * 16 + 12] = 0x10; /* GLOBAL NOTYPE */
            wr16(symtab, ns * 16 + 14, sh_data ? (uint16_t)sh_data : (uint16_t)0xFFF1);
            ns++;
            wr32(symtab, ns * 16, str_put(strs, &slen, sizeof strs, "__pex_rw_end"));
            wr32(symtab, ns * 16 + 4, rw_end);
            symtab[ns * 16 + 12] = 0x10;
            wr16(symtab, ns * 16 + 14, (uint16_t)sh_end);
            ns++;
        }
        /* output index of each input symbol: rescan symtab names is painful.
           Build a map while emitting by a second structure. Recompute: walk again
           storing into an int array parallel to lsyms. */
        {
            int map_out[MAX_SYM];
            int cursor;
            memset(map_out, 0xff, sizeof map_out);
            /* symbol 0 is the null entry; section symbols follow; named symbols match the emit loop. */
            cursor = 1 + 1 + (sec_sym_ro >= 0) + (sec_sym_data >= 0) + (sec_sym_bss >= 0);
            for (i = 0; i < 5; i++) if (have_meta[i]) cursor++;
            for (i = 0; i < nsym; i++) {
                Sym *s = &lsyms[i];
                int typ = s->info & 15;
                if (typ == 3 || typ == 4) continue;
                if (!s->name || !s->name[0] || s->name[0] == '$') continue;
                if (s->shndx == 0 || s->shndx >= 0xFF00) continue;
                if (s->sec < 0 || !lsecs[s->sec].live || out_of[s->sec] < 0) continue;
                map_out[i] = cursor++;
            }
            /* relocs */
            for (i = 0; i < nrel; i++) {
                Rel *r = &lrels[i];
                Sec *ts; int osym; uint32_t info; int oi;
                if (r->target < 0 || !lsecs[r->target].live || !lsecs[r->target].size) continue;
                if (r->def < 0) continue;
                ts = &lsecs[r->target];
                oi = out_of[r->target];
                if (oi < 1 || oi > 3) continue; /* only text/ro/data relocs; meta values already applied */
                if (r->type == 0 || r->type == 40) continue;
                osym = map_out[r->def];
                if (osym < 0) {
                    /* section symbol or skipped: point at the output section symbol */
                    int di = lsyms[r->def].sec >= 0 ? out_of[lsyms[r->def].sec] : -1;
                    if ((lsyms[r->def].info & 15) == 3 || osym < 0) {
                        if (di == 1) osym = sec_sym_text;
                        else if (di == 2) osym = sec_sym_ro;
                        else if (di == 3) osym = sec_sym_data;
                        else if (di == 4) osym = sec_sym_bss;
                    }
                }
                if (osym < 0) { die("cannot place reloc for '%s'", lsyms[r->def].name); return 0; }
                if (nro >= 2048) { die("too many output relocs"); return 0; }
                info = ((uint32_t)osym << 8) | r->type;
                wr32(relbuf, nro * 8, ts->out_addr + r->offset);
                wr32(relbuf, nro * 8 + 4, info);
                nro++;
            }
            (void)map_out;
        }
        /* write file */
        memset(elfout, 0, 64);
        off = 52;
        while (off & 3) off++;
        {
            uint32_t text_off = off;
            uint32_t ro_off = 0, data_off = 0, rel_off = 0, sym_off, str_off, shs_off, shoff;
            int sec_index = 0;
            uint32_t nm[16];
            int rel_sec_index, sym_sec_index, str_sec_index, sh_sec_index;
            if (off + otlen > sizeof elfout) { die("elf too large"); return 0; }
            memcpy(elfout + off, otext, otlen); off += otlen;
            if (emit_ro) {
                while (off & 3) off++;
                ro_off = off; memcpy(elfout + off, orodata, orlen); off += orlen;
            }
            if (emit_data) {
                while (off & 7) off++;
                data_off = off; memcpy(elfout + off, odata, odlen); off += odlen;
            }
            {
                int mi;
                uint32_t meta_off[5];
                for (mi = 0; mi < 5; mi++) if (have_meta[mi]) {
                    while (off & 3) off++;
                    meta_off[mi] = off;
                    memcpy(elfout + off, ometa[mi], omlen[mi]); off += omlen[mi];
                }
                if (nro) {
                    while (off & 3) off++;
                    rel_off = off;
                    memcpy(elfout + off, relbuf, (size_t)nro * 8); off += (uint32_t)nro * 8;
                }
                while (off & 3) off++;
                sym_off = off;
                memcpy(elfout + off, symtab, (size_t)ns * 16); off += (uint32_t)ns * 16;
                str_off = off;
                memcpy(elfout + off, strs, slen); off += slen;
                shstr[0] = 0; shlen = 1;
                nm[0] = str_put(shstr, &shlen, sizeof shstr, ".text");
                nm[1] = emit_ro ? str_put(shstr, &shlen, sizeof shstr, ".rodata") : 0;
                nm[2] = emit_data ? str_put(shstr, &shlen, sizeof shstr, ".data") : 0;
                nm[3] = emit_bss ? str_put(shstr, &shlen, sizeof shstr, ".bss") : 0;
                for (mi = 0; mi < 5; mi++) nm[4 + mi] = have_meta[mi] ? str_put(shstr, &shlen, sizeof shstr, meta_name[mi]) : 0;
                nm[9] = nro ? str_put(shstr, &shlen, sizeof shstr, ".rel.text") : 0;
                nm[10] = str_put(shstr, &shlen, sizeof shstr, ".symtab");
                nm[11] = str_put(shstr, &shlen, sizeof shstr, ".strtab");
                nm[12] = str_put(shstr, &shlen, sizeof shstr, ".shstrtab");
                shs_off = off;
                memcpy(elfout + off, shstr, shlen); off += shlen;
                while (off & 3) off++;
                shoff = off;
                nsec_out = 1 + 1 + emit_ro + emit_data + emit_bss;
                for (mi = 0; mi < 5; mi++) if (have_meta[mi]) nsec_out++;
                if (nro) nsec_out++;
                nsec_out += 3; /* sym str shstr */
                if (shoff + (uint32_t)nsec_out * 40 > sizeof elfout) { die("elf too large"); return 0; }
                sec_index = 1;
                /* .text */
                wr32(elfout, shoff + sec_index * 40, nm[0]);
                wr32(elfout, shoff + sec_index * 40 + 4, 1);
                wr32(elfout, shoff + sec_index * 40 + 8, 6);
                wr32(elfout, shoff + sec_index * 40 + 16, text_off);
                wr32(elfout, shoff + sec_index * 40 + 20, otlen);
                wr32(elfout, shoff + sec_index * 40 + 32, 4);
                sec_index++;
                if (emit_ro) {
                    wr32(elfout, shoff + sec_index * 40, nm[1]);
                    wr32(elfout, shoff + sec_index * 40 + 4, 1);
                    wr32(elfout, shoff + sec_index * 40 + 8, 2);
                    wr32(elfout, shoff + sec_index * 40 + 12, ro_addr);
                    wr32(elfout, shoff + sec_index * 40 + 16, ro_off);
                    wr32(elfout, shoff + sec_index * 40 + 20, orlen);
                    wr32(elfout, shoff + sec_index * 40 + 32, 4);
                    sec_index++;
                }
                if (emit_data) {
                    wr32(elfout, shoff + sec_index * 40, nm[2]);
                    wr32(elfout, shoff + sec_index * 40 + 4, 1);
                    wr32(elfout, shoff + sec_index * 40 + 8, 3);
                    wr32(elfout, shoff + sec_index * 40 + 12, data_addr);
                    wr32(elfout, shoff + sec_index * 40 + 16, data_off);
                    wr32(elfout, shoff + sec_index * 40 + 20, odlen);
                    wr32(elfout, shoff + sec_index * 40 + 32, 8);
                    sec_index++;
                }
                if (emit_bss) {
                    wr32(elfout, shoff + sec_index * 40, nm[3]);
                    wr32(elfout, shoff + sec_index * 40 + 4, 8);
                    wr32(elfout, shoff + sec_index * 40 + 8, 3);
                    wr32(elfout, shoff + sec_index * 40 + 12, data_end);
                    wr32(elfout, shoff + sec_index * 40 + 20, rw_end - data_end);
                    wr32(elfout, shoff + sec_index * 40 + 32, 4);
                    sec_index++;
                }
                for (mi = 0; mi < 5; mi++) if (have_meta[mi]) {
                    wr32(elfout, shoff + sec_index * 40, nm[4 + mi]);
                    wr32(elfout, shoff + sec_index * 40 + 4, 1);
                    wr32(elfout, shoff + sec_index * 40 + 8, 2);
                    wr32(elfout, shoff + sec_index * 40 + 12, 0x70000000u + (uint32_t)mi * 0x1000u);
                    wr32(elfout, shoff + sec_index * 40 + 16, meta_off[mi]);
                    wr32(elfout, shoff + sec_index * 40 + 20, omlen[mi]);
                    wr32(elfout, shoff + sec_index * 40 + 32, 4);
                    sec_index++;
                }
                rel_sec_index = sec_index;
                if (nro) {
                    int sym_will = sec_index + 1;
                    int text_si = 1;
                    wr32(elfout, shoff + sec_index * 40, nm[9]);
                    wr32(elfout, shoff + sec_index * 40 + 4, 9);
                    wr32(elfout, shoff + sec_index * 40 + 16, rel_off);
                    wr32(elfout, shoff + sec_index * 40 + 20, (uint32_t)nro * 8);
                    wr32(elfout, shoff + sec_index * 40 + 24, (uint32_t)sym_will);
                    wr32(elfout, shoff + sec_index * 40 + 28, (uint32_t)text_si);
                    wr32(elfout, shoff + sec_index * 40 + 32, 4);
                    wr32(elfout, shoff + sec_index * 40 + 36, 8);
                    sec_index++;
                }
                sym_sec_index = sec_index;
                {
                    int fg = 1;
                    for (i = 1; i < ns; i++) {
                        if ((symtab[i * 16 + 12] >> 4) == 0) fg = i + 1; else break;
                    }
                    wr32(elfout, shoff + sec_index * 40, nm[10]);
                    wr32(elfout, shoff + sec_index * 40 + 4, 2);
                    wr32(elfout, shoff + sec_index * 40 + 16, sym_off);
                    wr32(elfout, shoff + sec_index * 40 + 20, (uint32_t)ns * 16);
                    wr32(elfout, shoff + sec_index * 40 + 24, (uint32_t)(sec_index + 1));
                    wr32(elfout, shoff + sec_index * 40 + 28, (uint32_t)fg);
                    wr32(elfout, shoff + sec_index * 40 + 32, 4);
                    wr32(elfout, shoff + sec_index * 40 + 36, 16);
                    sec_index++;
                }
                str_sec_index = sec_index;
                wr32(elfout, shoff + sec_index * 40, nm[11]);
                wr32(elfout, shoff + sec_index * 40 + 4, 3);
                wr32(elfout, shoff + sec_index * 40 + 16, str_off);
                wr32(elfout, shoff + sec_index * 40 + 20, slen);
                sec_index++;
                sh_sec_index = sec_index;
                wr32(elfout, shoff + sec_index * 40, nm[12]);
                wr32(elfout, shoff + sec_index * 40 + 4, 3);
                wr32(elfout, shoff + sec_index * 40 + 16, shs_off);
                wr32(elfout, shoff + sec_index * 40 + 20, shlen);
                sec_index++;
                elfout[0] = 0x7f; elfout[1] = 'E'; elfout[2] = 'L'; elfout[3] = 'F';
                elfout[4] = 1; elfout[5] = 1; elfout[6] = 1;
                wr16(elfout, 16, 2);
                wr16(elfout, 18, 40);
                wr32(elfout, 20, 1);
                {
                    int pm = find_defined("pex_main");
                    uint32_t entry = pm >= 0 && lsyms[pm].sec >= 0 ? lsecs[lsyms[pm].sec].out_addr + lsyms[pm].value : 1;
                    wr32(elfout, 24, entry);
                }
                wr32(elfout, 32, shoff);
                wr32(elfout, 36, 0x05000000);
                wr16(elfout, 40, 52);
                wr16(elfout, 46, 40);
                wr16(elfout, 48, (uint16_t)sec_index);
                wr16(elfout, 50, (uint16_t)sh_sec_index);
                elfout_len = shoff + (uint32_t)sec_index * 40;
                (void)rel_sec_index; (void)sym_sec_index; (void)str_sec_index; (void)meta_off;
                return 1;
            }
        }
    }
}

/* ---------------------------------------------------------------- driver */

static void reset_compile(void) {
    static const char *pre[] = {
        "uint8_t","uint16_t","uint32_t","uint64_t","int8_t","int16_t","int32_t","int64_t",
        "size_t","ptrdiff_t","uintptr_t","intptr_t","va_list", NULL
    };
    int i;
    arena_used = 0; failed = 0; errbuf[0] = 0;
    nmac = 0; ntypedef = 0; ctlen = 0; crolen = 0; ncrel = 0; ncsym = 0;
    nlit = lit0 = 0; nbr = 0; nlab = 0;
    for (i = 0; pre[i]; i++) add_typedef(pre[i]);
    add_obj_mac("__GNUC__", "4");
}

static int compile_c(const char *src, uint32_t len) {
    char *buf;
    reset_compile();
    buf = aalloc(len + 1);
    if (!buf) return 0;
    memcpy(buf, src, len); buf[len] = 0;
    preprocess(buf, len);
    if (failed) return 0;
#ifndef PEXCC_WASM
    if (getenv("PEXCC_DUMP")) fwrite(ppout, 1, pplen, stderr);
#endif
    tokenize(ppout);
    if (failed) return 0;
    parse_file();
    if (failed) return 0;
    if (!csym_find("pex_main") || !csyms[csym_find("pex_main")].defined) { die("no pex_main"); return 0; }
    if (!write_object()) return 0;
    return 1;
}

static int build_from_source(const char *src, uint32_t len) {
    /* keep toolchain files; compile this buffer and link every .o/.a */
    if (!compile_c(src, len)) return 0;
    /* objmem is an extra object, not in the file table */
    {
        int i;
        /* link_all walks files[]. Prepend by temporarily... we'll call a variant.
           Easiest: stash obj as a file then link, then drop it. */
        if (add_file_mem("module.o", objmem, objlen)) return 0;
        files[nfiles - 1].kind = FK_O;
        i = link_all();
        nfiles--; /* drop module.o so the next build can add it again */
        return i;
    }
}

#ifdef PEXCC_WASM
__attribute__((export_name("pexcc_reset")))
void pexcc_reset(void) {
    nfiles = 0; files_used = 0; failed = 0; errbuf[0] = 0; elfout_len = 0;
}
__attribute__((export_name("pexcc_alloc")))
uint32_t pexcc_alloc(uint32_t n) {
    uint32_t p;
    n = (n + 3u) & ~3u;
    if (files_used + n > sizeof files_mem) return 0;
    p = (uint32_t)(uintptr_t)(files_mem + files_used);
    files_used += n;
    return p;
}
__attribute__((export_name("pexcc_add_file")))
int pexcc_add_file(uint32_t name, uint32_t data, uint32_t len) {
    const char *nm = (const char *)(uintptr_t)name;
    const uint8_t *d = (const uint8_t *)(uintptr_t)data;
    failed = 0;
    if (!nm || !d) { die("bad file"); return 1; }
    return add_file_mem(nm, d, len);
}
__attribute__((export_name("pexcc_build")))
int pexcc_build(uint32_t src, uint32_t len) {
    const char *s = (const char *)(uintptr_t)src;
    failed = 0; errbuf[0] = 0; elfout_len = 0;
    if (!s) { die("no source"); return 1; }
    return build_from_source(s, len) ? 0 : 1;
}
__attribute__((export_name("pexcc_out_ptr")))
uint32_t pexcc_out_ptr(void) { return (uint32_t)(uintptr_t)elfout; }
__attribute__((export_name("pexcc_out_len")))
uint32_t pexcc_out_len(void) { return elfout_len; }
__attribute__((export_name("pexcc_error")))
uint32_t pexcc_error(void) { return (uint32_t)(uintptr_t)errbuf; }
#else
static uint8_t *read_whole(const char *path, uint32_t *sz) {
    FILE *fp = fopen(path, "rb");
    long n; uint8_t *b;
    if (!fp) { die("cannot open %s", path); return NULL; }
    fseek(fp, 0, SEEK_END); n = ftell(fp); fseek(fp, 0, SEEK_SET);
    if (n < 0 || n > 8 * 1024 * 1024) { fclose(fp); die("file too large"); return NULL; }
    b = malloc((size_t)n + 1);
    if (!b || fread(b, 1, (size_t)n, fp) != (size_t)n) { fclose(fp); die("read %s", path); return NULL; }
    fclose(fp); b[n] = 0; *sz = (uint32_t)n; return b;
}

static void usage(void) {
    fprintf(stderr,
        "usage:\n"
        "  pexcc cc  -o out.elf [-I dir] file.c [extra.o extra.a]\n"
        "  pexcc link -o out.elf file.o [more.o libgcc.a]\n"
        "  pexcc pp  [-I dir] file.c\n");
}

int main(int argc, char **argv) {
    int i; const char *cmd, *outp = NULL, *srcp = NULL;
    if (argc < 2) { usage(); return 2; }
    cmd = argv[1];
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) { outp = argv[++i]; continue; }
        if (strcmp(argv[i], "-I") == 0 && i + 1 < argc) {
            if (nincdir < 16) snprintf(incdir[nincdir++], 256, "%s", argv[++i]);
            continue;
        }
        if (strcmp(argv[i], "--") == 0) continue;
        {
            uint32_t sz = 0; uint8_t *b;
            const char *p = argv[i];
            size_t n = strlen(p);
            if (n >= 2 && strcmp(p + n - 2, ".c") == 0) { srcp = p; continue; }
            b = read_whole(p, &sz);
            if (!b) { fprintf(stderr, "pexcc: %s\n", errbuf); return 1; }
            if (add_file_mem(p, b, sz)) { fprintf(stderr, "pexcc: %s\n", errbuf); return 1; }
        }
    }
    if (strcmp(cmd, "pp") == 0) {
        uint32_t sz = 0; uint8_t *b;
        if (!srcp) { usage(); return 2; }
        b = read_whole(srcp, &sz);
        if (!b || !compile_c((char *)b, sz)) { fprintf(stderr, "pexcc: %s\n", errbuf); return 1; }
        fwrite(ppout, 1, pplen, stdout);
        return 0;
    }
    if (strcmp(cmd, "cc") == 0) {
        uint32_t sz = 0; uint8_t *b; FILE *fp;
        if (!srcp || !outp) { usage(); return 2; }
        b = read_whole(srcp, &sz);
        if (!b || !build_from_source((char *)b, sz)) { fprintf(stderr, "pexcc: %s\n", errbuf); return 1; }
        fp = fopen(outp, "wb");
        if (!fp || fwrite(elfout, 1, elfout_len, fp) != elfout_len) { fprintf(stderr, "pexcc: write failed\n"); return 1; }
        fclose(fp);
        return 0;
    }
    if (strcmp(cmd, "link") == 0) {
        FILE *fp;
        if (!outp) { usage(); return 2; }
        if (!link_all()) { fprintf(stderr, "pexcc: %s\n", errbuf); return 1; }
        fp = fopen(outp, "wb");
        if (!fp || fwrite(elfout, 1, elfout_len, fp) != elfout_len) { fprintf(stderr, "pexcc: write failed\n"); return 1; }
        fclose(fp);
        return 0;
    }
    usage();
    return 2;
}
#endif
