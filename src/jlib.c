/* java.lang / java.util / java.io natives */
#include "plat.h"
#include <math.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>

static Class *C(const char *n) { return vm_class(n); }
static Obj *jstr(const char *s) { return vm_new_string_utf8(s); }
static int is_str(Obj *o) { return o && o->kind == K_STRING; }

/* ------------------------------------------------------------ Object etc -- */
NATIVE(n_nop) { (void)args; (void)ret; }
NATIVE(n_ret0) { (void)args; RETI(0); }
NATIVE(n_ret_this) { RETO(args[0].o); }
NATIVE(n_true) { (void)args; RETI(1); }

NATIVE(n_thr_init0) { (void)args; (void)ret; }
NATIVE(n_thr_init1) { Obj *t = args[0].o, *m = args[1].o; if (m && !t->data) t->data = vm_string_to_utf8(m); }
NATIVE(n_thr_print) { Obj *t = args[0].o; (void)ret; fprintf(stderr, "[java] %s: %s\n", t->cls->name, t->data ? (char *)t->data : ""); }

NATIVE(n_thread_sleep) { (void)ret; app_frame_hook(0); int64_t ms = args[0].j; if (ms > 10000) ms = 10000; if (ms > 0) plat_sleep_ms((int)ms); }
NATIVE(n_thread_yield) { (void)args; (void)ret; app_frame_hook(0); }
NATIVE(n_thread_start) { (void)ret; app_thread_start(args[0].o); }

NATIVE(n_sys_time) { (void)args; RETJ((int64_t)plat_ms()); }
NATIVE(n_sys_exit) { (void)ret; plat_quit(args[0].i); }
NATIVE(n_sys_arraycopy) {
    (void)ret; Obj *s = args[0].o, *d = args[2].o; int sp = args[1].i, dp = args[3].i, n = args[4].i;
    if (!s || !d) { vm_throw("Ljava/lang/NullPointerException;", "arraycopy"); return; }
    if (sp < 0 || dp < 0 || n < 0 || sp + n > s->len || dp + n > d->len) { vm_throw("Ljava/lang/ArrayIndexOutOfBoundsException;", "arraycopy"); return; }
    size_t es = s->esz == E_REF ? sizeof(Obj *) : s->esz;
    memmove((uint8_t *)d->data + dp * es, (uint8_t *)s->data + sp * es, n * es);
}
static Obj *mk_printstream(void) { return vm_new_obj(C("Ljava/io/PrintStream;")); }
NATIVE(n_println) { (void)ret; if (plat_log && args[1].o) { char *u = vm_string_to_utf8(args[1].o); fprintf(stderr, "[out] %s\n", u); free(u); } }

NATIVE(n_math_absi) { int x = args[0].i; RETI(x < 0 ? -x : x); }
NATIVE(n_math_maxi) { RETI(args[0].i > args[1].i ? args[0].i : args[1].i); }
NATIVE(n_math_mini) { RETI(args[0].i < args[1].i ? args[0].i : args[1].i); }
NATIVE(n_math_roundf) { float f = args[0].f; RETI(f != f ? 0 : (int32_t)floorf(f + 0.5f)); }
NATIVE(n_int_toString) { char b[16]; snprintf(b, sizeof b, "%d", args[0].i); RETO(jstr(b)); }
NATIVE(n_int_parse) {
    Obj *s = args[0].o; if (!s) { vm_throw("Ljava/lang/NumberFormatException;", "null"); return; }
    char *u = vm_string_to_utf8(s); char *e; errno = 0; long v = strtol(u, &e, 10);
    if (!*u || *e || errno) { vm_throw("Ljava/lang/NumberFormatException;", u); free(u); return; }
    free(u); RETI((int32_t)v);
}

/* ---------------------------------------------------------------- String -- */
NATIVE(n_str_init_str) { Obj *t = args[0].o, *s = args[1].o; (void)ret; if (!s) { vm_throw("Ljava/lang/NullPointerException;", "String(null)"); return; }
    t->kind = K_STRING; t->len = s->len; t->data = malloc(2 * (s->len ? s->len : 1)); memcpy(t->data, s->data, 2 * s->len); }
static void str_set(Obj *t, const uint16_t *c, int n) { t->kind = K_STRING; t->len = n; t->data = malloc(2 * (n ? n : 1)); if (n) memcpy(t->data, c, 2 * n); }
NATIVE(n_str_init_chars) { (void)ret; Obj *a = args[1].o; if (!a) { vm_throw("Ljava/lang/NullPointerException;", "String(char[])"); return; } str_set(args[0].o, ARR_C(a), a->len); }
NATIVE(n_str_init_chars3) { (void)ret; Obj *a = args[1].o; int o = args[2].i, n = args[3].i; if (!a || o < 0 || n < 0 || o + n > a->len) { vm_throw("Ljava/lang/IndexOutOfBoundsException;", "String(char[],int,int)"); return; } str_set(args[0].o, ARR_C(a) + o, n); }
NATIVE(n_str_charAt) { Obj *s = args[0].o; int i = args[1].i; if (i < 0 || i >= s->len) { vm_throw("Ljava/lang/StringIndexOutOfBoundsException;", "charAt"); return; } RETI(STR_C(s)[i]); }
NATIVE(n_str_length) { RETI(args[0].o->len); }
NATIVE(n_str_equals) { Obj *a = args[0].o, *b = args[1].o; RETI(a == b || (is_str(b) && a->len == b->len && !memcmp(a->data, b->data, 2 * a->len))); }
static int str_index(Obj *s, Obj *t, int from) {
    if (from < 0) from = 0;
    for (int i = from; i + t->len <= s->len; i++) if (!memcmp(STR_C(s) + i, STR_C(t), 2 * t->len)) return i;
    return -1;
}
NATIVE(n_str_indexOf) { if (!args[1].o) { vm_throw("Ljava/lang/NullPointerException;", "indexOf"); return; } RETI(str_index(args[0].o, args[1].o, 0)); }
NATIVE(n_str_startsWith) { Obj *s = args[0].o, *p = args[1].o; if (!p) { vm_throw("Ljava/lang/NullPointerException;", "startsWith"); return; } RETI(p->len <= s->len && !memcmp(s->data, p->data, 2 * p->len)); }
NATIVE(n_str_substring1) { Obj *s = args[0].o; int b = args[1].i; if (b < 0 || b > s->len) { vm_throw("Ljava/lang/StringIndexOutOfBoundsException;", "substring"); return; } RETO(vm_new_string_u16(STR_C(s) + b, s->len - b)); }
NATIVE(n_str_substring2) { Obj *s = args[0].o; int b = args[1].i, e = args[2].i; if (b < 0 || e > s->len || b > e) { vm_throw("Ljava/lang/StringIndexOutOfBoundsException;", "substring"); return; } RETO(vm_new_string_u16(STR_C(s) + b, e - b)); }
NATIVE(n_str_toCharArray) { Obj *s = args[0].o; Obj *a = vm_new_array_t("[C", s->len); if (s->len) memcpy(a->data, s->data, 2 * s->len); RETO(a); }
NATIVE(n_str_valueOf) {
    Obj *o = args[0].o;
    if (!o) { RETO(jstr("null")); return; }
    if (is_str(o)) { RETO(o); return; }
    RETO(jstr(o->cls->name));
}
NATIVE(n_str_hashCode) { Obj *s = args[0].o; int32_t h = 0; for (int i = 0; i < s->len; i++) h = 31 * h + STR_C(s)[i]; RETI(h); }
NATIVE(n_str_toString) { RETO(args[0].o); }

/* replaceAll: supports literal patterns and the few regex forms the game uses */
static int rx_match_here(const uint16_t *p, int pn, const uint16_t *s, int sn, int *consumed);
NATIVE(n_str_replaceAll) {
    Obj *s = args[0].o, *pat = args[1].o, *rep = args[2].o;
    if (!pat || !rep) { vm_throw("Ljava/lang/NullPointerException;", "replaceAll"); return; }
    uint16_t *out = malloc(2 * (s->len * (rep->len + 1) + 8)); int n = 0, i = 0;
    while (i <= s->len) {
        int used = 0;
        int m = pat->len > 0 && rx_match_here(STR_C(pat), pat->len, STR_C(s) + i, s->len - i, &used) && used > 0;
        if (m) { memcpy(out + n, STR_C(rep), 2 * rep->len); n += rep->len; i += used; }
        else { if (i < s->len) out[n++] = STR_C(s)[i]; i++; }
    }
    RETO(vm_new_string_u16(out, n)); free(out);
}
/* tiny regex: literals, \x escapes, \s \d \w, '.', [...] classes (with ranges, ^), quantifiers + * ? on a single atom */
static int atom_len(const uint16_t *p, int pn) {
    if (p[0] == '\\') return pn > 1 ? 2 : 1;
    if (p[0] == '[') { int i = 1; if (i < pn && p[i] == '^') i++; if (i < pn && p[i] == ']') i++; while (i < pn && p[i] != ']') { if (p[i] == '\\') i++; i++; } return i < pn ? i + 1 : pn; }
    return 1;
}
static int atom_match(const uint16_t *p, int al, uint16_t c) {
    if (p[0] == '.') return 1;
    if (p[0] == '\\' && al > 1) {
        switch (p[1]) { case 's': return c == ' ' || (c >= 9 && c <= 13); case 'd': return c >= '0' && c <= '9';
            case 'w': return c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
            case 'n': return c == '\n'; case 't': return c == '\t'; case 'r': return c == '\r'; default: return c == p[1]; }
    }
    if (p[0] == '[') {
        int i = 1, neg = 0, hit = 0; if (p[i] == '^') { neg = 1; i++; }
        for (; i < al - 1; i++) {
            uint16_t a = p[i]; if (a == '\\' && i + 1 < al - 1) { i++; a = p[i]; if (a == 's') { hit |= (c == ' ' || (c >= 9 && c <= 13)); continue; } if (a == 'd') { hit |= (c >= '0' && c <= '9'); continue; } if (a == 'w') { hit |= (c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')); continue; } if (a == 'n') a = '\n'; else if (a == 't') a = '\t'; }
            if (i + 2 < al - 1 && p[i + 1] == '-') { if (c >= a && c <= p[i + 2]) hit = 1; i += 2; } else if (c == a) hit = 1;
        }
        return neg ? !hit : hit;
    }
    return c == p[0];
}
static int rx_match_here(const uint16_t *p, int pn, const uint16_t *s, int sn, int *consumed) {
    if (pn == 0) { *consumed = 0; return 1; }
    int al = atom_len(p, pn); int q = al < pn ? p[al] : 0;
    if (q == '+' || q == '*' || q == '?') {
        int max = q == '?' ? 1 : sn, min = q == '+' ? 1 : 0, k = 0;
        while (k < max && k < sn && atom_match(p, al, s[k])) k++;
        for (; k >= min; k--) { int c2; if (rx_match_here(p + al + 1, pn - al - 1, s + k, sn - k, &c2)) { *consumed = k + c2; return 1; } }
        return 0;
    }
    if (sn > 0 && atom_match(p, al, s[0])) { int c2; if (rx_match_here(p + al, pn - al, s + 1, sn - 1, &c2)) { *consumed = 1 + c2; return 1; } }
    return 0;
}

/* ---------------------------------------------------------- StringBuilder -- */
typedef struct { int len, cap; uint16_t c[1]; } SB;
static SB *sb_get(Obj *o) {
    if (!o->data) { o->cap = 32; SB *b = malloc(sizeof(SB) + 2 * 32); b->len = 0; b->cap = 32; o->data = b; }
    return o->data;
}
static void sb_add(Obj *o, const uint16_t *c, int n) {
    SB *b = sb_get(o);
    if (b->len + n > b->cap) { int nc = (b->len + n) * 2; b = realloc(b, sizeof(SB) + 2 * nc); b->cap = nc; o->data = b; }
    memcpy(b->c + b->len, c, 2 * n); b->len += n;
}
static void sb_adda(Obj *o, const char *s) { uint16_t t[64]; int n = 0; while (s[n] && n < 64) { t[n] = (uint8_t)s[n]; n++; } sb_add(o, t, n); }
NATIVE(n_sb_init) { (void)ret; sb_get(args[0].o); }
NATIVE(n_sb_init_s) { (void)ret; Obj *s = args[1].o; sb_get(args[0].o); if (s) sb_add(args[0].o, STR_C(s), s->len); else vm_throw("Ljava/lang/NullPointerException;", "StringBuilder(null)"); }
NATIVE(n_sb_app_s) { Obj *s = args[1].o; if (s) sb_add(args[0].o, STR_C(s), s->len); else sb_adda(args[0].o, "null"); RETO(args[0].o); }
NATIVE(n_sb_app_c) { uint16_t c = (uint16_t)args[1].i; sb_add(args[0].o, &c, 1); RETO(args[0].o); }
NATIVE(n_sb_app_i) { char b[16]; snprintf(b, sizeof b, "%d", args[1].i); sb_adda(args[0].o, b); RETO(args[0].o); }
NATIVE(n_sb_app_o) { Obj *s = args[1].o; if (!s) sb_adda(args[0].o, "null"); else if (is_str(s)) sb_add(args[0].o, STR_C(s), s->len); else { uint16_t t[96]; int n = 0; const char *nm = s->cls->name; while (nm[n] && n < 96) { t[n] = nm[n]; n++; } sb_add(args[0].o, t, n); } RETO(args[0].o); }
NATIVE(n_sb_toString) { SB *b = sb_get(args[0].o); RETO(vm_new_string_u16(b->c, b->len)); }

/* ----------------------------------------------------------------- Vector -- */
static void vec_init(Obj *o) { o->kind = K_ARRAY; o->esz = E_REF; o->cap = 8; o->len = 0; o->data = calloc(8, sizeof(Obj *)); }
NATIVE(n_vec_init) { (void)ret; vec_init(args[0].o); }
NATIVE(n_vec_add) { Obj *v = args[0].o; if (v->len == v->cap) { v->cap *= 2; v->data = realloc(v->data, v->cap * sizeof(Obj *)); } ARR_R(v)[v->len++] = args[1].o; RETI(1); }
NATIVE(n_vec_get) { Obj *v = args[0].o; int i = args[1].i; if (i < 0 || i >= v->len) { vm_throw("Ljava/lang/ArrayIndexOutOfBoundsException;", "Vector.get"); return; } RETO(ARR_R(v)[i]); }
NATIVE(n_vec_set) { Obj *v = args[0].o; int i = args[1].i; if (i < 0 || i >= v->len) { vm_throw("Ljava/lang/ArrayIndexOutOfBoundsException;", "Vector.set"); return; } Obj *old = ARR_R(v)[i]; ARR_R(v)[i] = args[2].o; RETO(old); }
NATIVE(n_vec_size) { RETI(args[0].o->len); }
NATIVE(n_vec_clear) { (void)ret; Obj *v = args[0].o; memset(v->data, 0, v->len * sizeof(Obj *)); v->len = 0; }

/* ----------------------------------------------------------------- Random -- */
NATIVE(n_rnd_init) { (void)ret; args[0].o->f[0].j = (args[1].j ^ 0x5DEECE66DLL) & ((1LL << 48) - 1); }
NATIVE(n_rnd_nextInt) { Obj *r = args[0].o; int64_t s = (r->f[0].j * 0x5DEECE66DLL + 0xBLL) & ((1LL << 48) - 1); r->f[0].j = s; RETI((int32_t)(s >> 16)); }

/* ----------------------------------------------------------------- streams -- */
typedef struct { size_t len, pos; uint8_t b[1]; } IS;
typedef struct { size_t len, cap; char *path; uint8_t b[1]; } OS;
static IS *is_of(Obj *o) { while (o && !o->data && o->nf && o->f[0].o) o = o->f[0].o; return o ? (IS *)o->data : NULL; }
static OS *os_of(Obj *o) { while (o && !o->data && o->nf && o->f[0].o) o = o->f[0].o; return o ? (OS *)o->data : NULL; }
Obj *jlib_new_instream(const uint8_t *d, size_t n) {
    Obj *o = vm_new_obj(C("Ljava/io/ByteArrayInputStream;"));
    IS *s = malloc(sizeof(IS) + n); s->len = n; s->pos = 0; if (n) memcpy(s->b, d, n); o->data = s; return o;
}
static Obj *new_outstream(const char *path) {
    Obj *o = vm_new_obj(C(path ? "Ljava/io/FileOutputStream;" : "Ljava/io/ByteArrayOutputStream;"));
    OS *s = malloc(sizeof(OS) + 64); s->len = 0; s->cap = 64; s->path = path ? strdup(path) : NULL; o->data = s; return o;
}
static int need(Obj *o, size_t n) { IS *s = is_of(o); if (!s || s->pos + n > s->len) { vm_throw("Ljava/io/EOFException;", "eof"); return 0; } return 1; }
NATIVE(n_in_init_bytes) { (void)ret; Obj *b = args[1].o; if (!b) { vm_throw("Ljava/lang/NullPointerException;", "ByteArrayInputStream"); return; }
    IS *s = malloc(sizeof(IS) + b->len); s->len = b->len; s->pos = 0; memcpy(s->b, b->data, b->len); args[0].o->data = s; }
NATIVE(n_in_init_wrap) { (void)ret; if (args[0].o->nf) args[0].o->f[0].o = args[1].o; }
NATIVE(n_in_available) { IS *s = is_of(args[0].o); RETI(s ? (int)(s->len - s->pos) : 0); }
NATIVE(n_in_close) { (void)args; (void)ret; }
NATIVE(n_in_read) { IS *s = is_of(args[0].o); RETI(s && s->pos < s->len ? s->b[s->pos++] : -1); }
static void rd_into(Val *args, Val *ret, int hasrange) {
    IS *s = is_of(args[0].o); Obj *a = args[1].o; if (!a || !s) { vm_throw("Ljava/lang/NullPointerException;", "read"); return; }
    int off = hasrange ? args[2].i : 0, n = hasrange ? args[3].i : a->len;
    if (off < 0 || n < 0 || off + n > a->len) { vm_throw("Ljava/lang/IndexOutOfBoundsException;", "read"); return; }
    if (n == 0) { RETI(0); return; }
    if (s->pos >= s->len) { RETI(-1); return; }
    size_t k = s->len - s->pos; if ((size_t)n < k) k = n;
    memcpy(ARR_U8(a) + off, s->b + s->pos, k); s->pos += k; RETI((int)k);
}
NATIVE(n_in_read_a) { rd_into(args, ret, 0); }
NATIVE(n_in_read_a3) { rd_into(args, ret, 1); }
NATIVE(n_in_readFully) { (void)ret; Obj *a = args[1].o; if (!a) { vm_throw("Ljava/lang/NullPointerException;", "readFully"); return; } if (!need(args[0].o, a->len)) return; IS *s = is_of(args[0].o); memcpy(a->data, s->b + s->pos, a->len); s->pos += a->len; }
NATIVE(n_in_readBool) { if (!need(args[0].o, 1)) return; IS *s = is_of(args[0].o); RETI(s->b[s->pos++] != 0); }
NATIVE(n_in_readByte) { if (!need(args[0].o, 1)) return; IS *s = is_of(args[0].o); RETI((int8_t)s->b[s->pos++]); }
NATIVE(n_in_readUByte) { if (!need(args[0].o, 1)) return; IS *s = is_of(args[0].o); RETI(s->b[s->pos++]); }
NATIVE(n_in_readShort) { if (!need(args[0].o, 2)) return; IS *s = is_of(args[0].o); int v = (int16_t)(s->b[s->pos] << 8 | s->b[s->pos + 1]); s->pos += 2; RETI(v); }
NATIVE(n_in_readInt) { if (!need(args[0].o, 4)) return; IS *s = is_of(args[0].o); uint32_t v = (uint32_t)s->b[s->pos] << 24 | s->b[s->pos + 1] << 16 | s->b[s->pos + 2] << 8 | s->b[s->pos + 3]; s->pos += 4; RETI((int32_t)v); }
NATIVE(n_in_readUTF) {
    if (!need(args[0].o, 2)) return; IS *s = is_of(args[0].o);
    size_t n = s->b[s->pos] << 8 | s->b[s->pos + 1]; s->pos += 2;
    if (!need(args[0].o, n)) return;
    char *t = malloc(n + 1); memcpy(t, s->b + s->pos, n); t[n] = 0; s->pos += n;
    RETO(vm_new_string_utf8(t)); free(t);
}

static void os_put(Obj *o, const void *d, size_t n) {
    OS *s = os_of(o); if (!s) return;
    Obj *root = o; while (root && !root->data) root = root->f[0].o;
    if (s->len + n > s->cap) { size_t nc = (s->len + n) * 2; s = realloc(s, sizeof(OS) + nc); s->cap = nc; root->data = s; }
    memcpy(s->b + s->len, d, n); s->len += n;
}
static void os_flush_file(Obj *o) {
    OS *s = os_of(o); if (!s || !s->path) return;
    FILE *f = fopen(s->path, "wb"); if (!f) { vm_throw("Ljava/io/IOException;", "cannot write file"); return; }
    fwrite(s->b, 1, s->len, f); fclose(f);
}
NATIVE(n_out_init) { (void)ret; Obj *o = args[0].o; OS *s = malloc(sizeof(OS) + 64); s->len = 0; s->cap = 64; s->path = NULL; o->data = s; }
NATIVE(n_out_init_wrap) { (void)ret; if (args[0].o->nf) args[0].o->f[0].o = args[1].o; }
NATIVE(n_out_size) { OS *s = os_of(args[0].o); RETI(s ? (int)s->len : 0); }
NATIVE(n_out_toByteArray) { OS *s = os_of(args[0].o); Obj *a = vm_new_array_t("[B", s ? (int)s->len : 0); if (s && s->len) memcpy(a->data, s->b, s->len); RETO(a); }
NATIVE(n_out_write_a3) { (void)ret; Obj *a = args[1].o; int off = args[2].i, n = args[3].i; if (!a || off < 0 || n < 0 || off + n > a->len) { vm_throw("Ljava/lang/IndexOutOfBoundsException;", "write"); return; } os_put(args[0].o, ARR_U8(a) + off, n); }
NATIVE(n_out_write_a) { (void)ret; Obj *a = args[1].o; if (!a) { vm_throw("Ljava/lang/NullPointerException;", "write"); return; } os_put(args[0].o, a->data, a->len); }
NATIVE(n_out_writeBool) { (void)ret; uint8_t b = args[1].i != 0; os_put(args[0].o, &b, 1); }
NATIVE(n_out_writeInt) { (void)ret; uint32_t v = args[1].u; uint8_t b[4] = {v >> 24, v >> 16, v >> 8, v}; os_put(args[0].o, b, 4); }
NATIVE(n_out_close) { (void)ret; os_flush_file(args[0].o); }

/* ------------------------------------------------------- app file storage -- */
static void data_path(Obj *name, char *buf, size_t n) {
    char *u = vm_string_to_utf8(name); for (char *p = u; *p; p++) if (*p == '/' || *p == '\\') *p = '_';
    snprintf(buf, n, "%s%s", plat_data_dir(), u); free(u);
}
NATIVE(n_act_openFileInput) {
    if (!args[1].o) { vm_throw("Ljava/lang/NullPointerException;", "openFileInput"); return; }
    char p[512]; data_path(args[1].o, p, sizeof p); FILE *f = fopen(p, "rb");
    if (!f) { vm_throw("Ljava/io/FileNotFoundException;", p); return; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); uint8_t *b = malloc(n + 1);
    if (fread(b, 1, n, f) != (size_t)n) n = 0;
    fclose(f); RETO(jlib_new_instream(b, n)); free(b);
}
NATIVE(n_act_openFileOutput) {
    if (!args[1].o) { vm_throw("Ljava/lang/NullPointerException;", "openFileOutput"); return; }
    char p[512]; data_path(args[1].o, p, sizeof p); RETO(new_outstream(p));
}
NATIVE(n_act_deleteFile) { char p[512]; data_path(args[1].o, p, sizeof p); RETI(unlink(p) == 0); }
Obj *jlib_new_string_array(char **names, int n) {
    Obj *a = vm_new_array_t("[Ljava/lang/String;", n);
    for (int i = 0; i < n; i++) ARR_R(a)[i] = vm_new_string_utf8(names[i]);
    return a;
}
NATIVE(n_act_fileList) {
    (void)args; DIR *d = opendir(plat_data_dir()); char *names[256]; int n = 0;
    if (d) { struct dirent *e; while ((e = readdir(d)) && n < 256) if (e->d_name[0] != '.') names[n++] = strdup(e->d_name); closedir(d); }
    Obj *a = jlib_new_string_array(names, n); for (int i = 0; i < n; i++) free(names[i]);
    RETO(a);
}

static const NativeClassDef jclasses[] = {
    {"Ljava/util/Random;", NULL, "seed"}, {"Ljava/io/DataInputStream;", NULL, "inner"}, {"Ljava/io/DataOutputStream;", NULL, "inner"},
    {NULL, NULL, NULL}};
static const NativeDef jnatives[] = {
    {"Ljava/lang/Object;.<init>()V", n_nop},
    {"Ljava/lang/Thread;.<init>()V", n_nop}, {"Ljava/lang/Thread;.sleep(J)V", n_thread_sleep}, {"Ljava/lang/Thread;.start()V", n_thread_start},
    {"Ljava/lang/Thread;.yield()V", n_thread_yield}, {"Ljava/lang/Thread;.run()V", n_nop},
    {"Ljava/lang/Throwable;.<init>()V", n_thr_init0}, {"Ljava/lang/Throwable;.<init>(Ljava/lang/String;)V", n_thr_init1},
    {"Ljava/lang/Throwable;.printStackTrace()V", n_thr_print},
    {"Ljava/lang/System;.currentTimeMillis()J", n_sys_time}, {"Ljava/lang/System;.exit(I)V", n_sys_exit}, {"Ljava/lang/System;.gc()V", n_nop},
    {"Ljava/lang/System;.arraycopy(Ljava/lang/Object;ILjava/lang/Object;II)V", n_sys_arraycopy},
    {"Ljava/io/PrintStream;.println(Ljava/lang/String;)V", n_println},
    {"Ljava/lang/Math;.abs(I)I", n_math_absi}, {"Ljava/lang/Math;.max(II)I", n_math_maxi}, {"Ljava/lang/Math;.min(II)I", n_math_mini},
    {"Ljava/lang/Math;.round(F)I", n_math_roundf},
    {"Ljava/lang/Integer;.toString(I)Ljava/lang/String;", n_int_toString}, {"Ljava/lang/Integer;.parseInt(Ljava/lang/String;)I", n_int_parse},
    {"Ljava/lang/String;.<init>(Ljava/lang/String;)V", n_str_init_str}, {"Ljava/lang/String;.<init>([C)V", n_str_init_chars},
    {"Ljava/lang/String;.<init>([CII)V", n_str_init_chars3}, {"Ljava/lang/String;.charAt(I)C", n_str_charAt},
    {"Ljava/lang/String;.length()I", n_str_length}, {"Ljava/lang/String;.equals(Ljava/lang/Object;)Z", n_str_equals},
    {"Ljava/lang/String;.indexOf(Ljava/lang/String;)I", n_str_indexOf}, {"Ljava/lang/String;.startsWith(Ljava/lang/String;)Z", n_str_startsWith},
    {"Ljava/lang/String;.substring(I)Ljava/lang/String;", n_str_substring1}, {"Ljava/lang/String;.substring(II)Ljava/lang/String;", n_str_substring2},
    {"Ljava/lang/String;.toCharArray()[C", n_str_toCharArray}, {"Ljava/lang/String;.valueOf(Ljava/lang/Object;)Ljava/lang/String;", n_str_valueOf},
    {"Ljava/lang/String;.replaceAll(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", n_str_replaceAll},
    {"Ljava/lang/String;.hashCode()I", n_str_hashCode}, {"Ljava/lang/String;.toString()Ljava/lang/String;", n_str_toString},
    {"Ljava/lang/StringBuilder;.<init>()V", n_sb_init}, {"Ljava/lang/StringBuilder;.<init>(Ljava/lang/String;)V", n_sb_init_s},
    {"Ljava/lang/StringBuilder;.append(Ljava/lang/String;)Ljava/lang/StringBuilder;", n_sb_app_s},
    {"Ljava/lang/StringBuilder;.append(Ljava/lang/String;)Ljava/lang/StringBuffer;", n_sb_app_s},
    {"Ljava/lang/StringBuilder;.append(C)Ljava/lang/StringBuilder;", n_sb_app_c}, {"Ljava/lang/StringBuilder;.append(I)Ljava/lang/StringBuilder;", n_sb_app_i},
    {"Ljava/lang/StringBuilder;.append(Ljava/lang/Object;)Ljava/lang/StringBuilder;", n_sb_app_o},
    {"Ljava/lang/StringBuilder;.toString()Ljava/lang/String;", n_sb_toString},
    {"Ljava/util/Vector;.<init>()V", n_vec_init}, {"Ljava/util/Vector;.add(Ljava/lang/Object;)Z", n_vec_add},
    {"Ljava/util/Vector;.get(I)Ljava/lang/Object;", n_vec_get}, {"Ljava/util/Vector;.set(ILjava/lang/Object;)Ljava/lang/Object;", n_vec_set},
    {"Ljava/util/Vector;.size()I", n_vec_size}, {"Ljava/util/Vector;.clear()V", n_vec_clear}, {"Ljava/util/Vector;.trimToSize()V", n_nop},
    {"Ljava/util/Random;.<init>(J)V", n_rnd_init}, {"Ljava/util/Random;.nextInt()I", n_rnd_nextInt},
    {"Ljava/io/DataInputStream;.<init>([B)V", n_in_init_bytes}, {"Ljava/io/DataInputStream;.<init>(Ljava/io/InputStream;)V", n_in_init_wrap},
    {"Ljava/io/DataInputStream;.available()I", n_in_available}, {"Ljava/io/DataInputStream;.close()V", n_in_close},
    {"Ljava/io/DataInputStream;.read()I", n_in_read}, {"Ljava/io/DataInputStream;.read([B)I", n_in_read_a}, {"Ljava/io/DataInputStream;.read([BII)I", n_in_read_a3},
    {"Ljava/io/DataInputStream;.readBoolean()Z", n_in_readBool}, {"Ljava/io/DataInputStream;.readByte()B", n_in_readByte},
    {"Ljava/io/DataInputStream;.readFully([B)V", n_in_readFully}, {"Ljava/io/DataInputStream;.readInt()I", n_in_readInt},
    {"Ljava/io/DataInputStream;.readShort()S", n_in_readShort}, {"Ljava/io/DataInputStream;.readUTF()Ljava/lang/String;", n_in_readUTF},
    {"Ljava/io/DataInputStream;.readUnsignedByte()I", n_in_readUByte},
    {"Ljava/io/DataOutputStream;.<init>()V", n_out_init}, {"Ljava/io/DataOutputStream;.<init>(Ljava/io/OutputStream;)V", n_out_init_wrap},
    {"Ljava/io/DataOutputStream;.size()I", n_out_size}, {"Ljava/io/DataOutputStream;.toByteArray()[B", n_out_toByteArray},
    {"Ljava/io/DataOutputStream;.write([BII)V", n_out_write_a3}, {"Ljava/io/DataOutputStream;.write([B)V", n_out_write_a},
    {"Ljava/io/DataOutputStream;.writeBoolean(Z)V", n_out_writeBool}, {"Ljava/io/DataOutputStream;.writeInt(I)V", n_out_writeInt},
    {"Ljava/io/DataOutputStream;.close()V", n_out_close}, {"Ljava/io/DataOutputStream;.flush()V", n_out_close},
    {"Landroid/app/Activity;.openFileInput(Ljava/lang/String;)Ljava/io/FileInputStream;", n_act_openFileInput},
    {"Landroid/app/Activity;.openFileOutput(Ljava/lang/String;I)Ljava/io/FileOutputStream;", n_act_openFileOutput},
    {"Landroid/app/Activity;.deleteFile(Ljava/lang/String;)Z", n_act_deleteFile}, {"Landroid/app/Activity;.fileList()[Ljava/lang/String;", n_act_fileList},
    {NULL, NULL}};

void jlib_register(void) {
    vm_register_native_classes(jclasses); vm_register_natives(jnatives);
    vm_register_statics("Ljava/lang/System;.out", mk_printstream); vm_register_statics("Ljava/lang/System;.err", mk_printstream);
    (void)n_ret0; (void)n_ret_this; (void)n_true;
}
