#include "res.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ----------------------------------------------------------------- inflate -- */
typedef struct { const uint8_t *in; size_t inlen, inpos; uint32_t bb; int bc; uint8_t *out; size_t cap, pos; int err; } St;
typedef struct { short count[16]; short symbol[288]; } Huff;
static int getbits(St *s, int need) {
    uint32_t v = s->bb;
    while (s->bc < need) { if (s->inpos >= s->inlen) { s->err = 1; return 0; } v |= (uint32_t)s->in[s->inpos++] << s->bc; s->bc += 8; }
    s->bb = v >> need; s->bc -= need; return v & ((1u << need) - 1);
}
static int hdecode(St *s, const Huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= getbits(s, 1); int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
        if (s->err) return -1;
    }
    return -1;
}
static int construct(Huff *h, const short *length, int n) {
    short offs[16]; memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[length[i]]++;
    if (h->count[0] == n) return 0;
    offs[1] = 0; for (int l = 1; l < 15; l++) offs[l + 1] = offs[l] + h->count[l];
    for (int i = 0; i < n; i++) if (length[i]) h->symbol[offs[length[i]]++] = i;
    return 0;
}
static void put(St *s, uint8_t b) {
    if (s->pos >= s->cap) { s->cap = s->cap ? s->cap * 2 : 4096; s->out = realloc(s->out, s->cap); }
    s->out[s->pos++] = b;
}
static const short lbase[] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const short lext[]  = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const short dbase[] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const short dext[]  = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
static int codes(St *s, const Huff *lc, const Huff *dc) {
    for (;;) {
        int sym = hdecode(s, lc);
        if (sym < 0 || s->err) return -1;
        if (sym < 256) put(s, sym);
        else if (sym == 256) return 0;
        else {
            sym -= 257; if (sym >= 29) return -1;
            int len = lbase[sym] + getbits(s, lext[sym]);
            int ds = hdecode(s, dc); if (ds < 0 || ds >= 30) return -1;
            size_t dist = dbase[ds] + getbits(s, dext[ds]);
            if (dist > s->pos) return -1;
            for (int i = 0; i < len; i++) put(s, s->out[s->pos - dist]);
        }
    }
}
uint8_t *inflate_raw(const uint8_t *src, size_t slen, size_t hint, size_t *outlen) {
    St s; memset(&s, 0, sizeof s); s.in = src; s.inlen = slen;
    if (hint) { s.cap = hint + 16; s.out = malloc(s.cap); }
    int last;
    do {
        last = getbits(&s, 1); int type = getbits(&s, 2);
        if (s.err) goto fail;
        if (type == 0) {
            s.bb = 0; s.bc = 0;
            if (s.inpos + 4 > s.inlen) goto fail;
            unsigned len = s.in[s.inpos] | s.in[s.inpos + 1] << 8; s.inpos += 4;
            if (s.inpos + len > s.inlen) goto fail;
            for (unsigned i = 0; i < len; i++) put(&s, s.in[s.inpos++]);
        } else if (type == 1) {
            static Huff lc, dc; static int built;
            if (!built) {
                short l[288]; int i = 0;
                for (; i < 144; i++) l[i] = 8; for (; i < 256; i++) l[i] = 9; for (; i < 280; i++) l[i] = 7; for (; i < 288; i++) l[i] = 8;
                construct(&lc, l, 288); for (i = 0; i < 30; i++) l[i] = 5; construct(&dc, l, 30); built = 1;
            }
            if (codes(&s, &lc, &dc)) goto fail;
        } else if (type == 2) {
            int nlen = getbits(&s, 5) + 257, ndist = getbits(&s, 5) + 1, ncode = getbits(&s, 4) + 4;
            static const short order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            short lengths[320]; memset(lengths, 0, sizeof lengths);
            for (int i = 0; i < ncode; i++) lengths[order[i]] = getbits(&s, 3);
            Huff lencode, lc, dc; construct(&lencode, lengths, 19);
            int idx = 0; short l2[320]; memset(l2, 0, sizeof l2);
            while (idx < nlen + ndist) {
                int sym = hdecode(&s, &lencode); if (sym < 0) goto fail;
                if (sym < 16) l2[idx++] = sym;
                else {
                    int len = 0, rep;
                    if (sym == 16) { if (!idx) goto fail; len = l2[idx - 1]; rep = 3 + getbits(&s, 2); }
                    else if (sym == 17) rep = 3 + getbits(&s, 3); else rep = 11 + getbits(&s, 7);
                    if (idx + rep > nlen + ndist) goto fail;
                    while (rep--) l2[idx++] = len;
                }
            }
            construct(&lc, l2, nlen); construct(&dc, l2 + nlen, ndist);
            if (codes(&s, &lc, &dc)) goto fail;
        } else goto fail;
    } while (!last);
    *outlen = s.pos; return s.out;
fail:
    free(s.out); return NULL;
}

/* --------------------------------------------------------------------- zip -- */
static uint8_t *zdata; static size_t zlen;
typedef struct { char *name; uint32_t off, csize, usize; int method; } ZE;
static ZE *zents; static int nz;
static uint32_t r32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t r16(const uint8_t *p) { return p[0] | p[1] << 8; }
int zip_open_file(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    fseek(f, 0, SEEK_END); zlen = ftell(f); fseek(f, 0, SEEK_SET);
    zdata = malloc(zlen); if (fread(zdata, 1, zlen, f) != zlen) { fclose(f); return -1; } fclose(f);
    long e = (long)zlen - 22;
    for (; e >= 0; e--) if (r32(zdata + e) == 0x06054b50) break;
    if (e < 0) return -2;
    nz = r16(zdata + e + 10); uint32_t cd = r32(zdata + e + 16);
    zents = calloc(nz, sizeof(ZE));
    const uint8_t *p = zdata + cd;
    for (int i = 0; i < nz; i++) {
        if (r32(p) != 0x02014b50) return -3;
        zents[i].method = r16(p + 10); zents[i].csize = r32(p + 20); zents[i].usize = r32(p + 24);
        uint32_t nl = r16(p + 28), el = r16(p + 30), cl = r16(p + 32); zents[i].off = r32(p + 42);
        zents[i].name = malloc(nl + 1); memcpy(zents[i].name, p + 46, nl); zents[i].name[nl] = 0;
        p += 46 + nl + el + cl;
    }
    return 0;
}
static ZE *zfind(const char *n) { for (int i = 0; i < nz; i++) if (!strcmp(zents[i].name, n)) return &zents[i]; return NULL; }
int zip_exists(const char *n) { return zfind(n) != NULL; }
uint8_t *zip_extract(const char *name, size_t *outlen) {
    ZE *e = zfind(name); if (!e) return NULL;
    const uint8_t *lh = zdata + e->off; if (r32(lh) != 0x04034b50) return NULL;
    const uint8_t *d = lh + 30 + r16(lh + 26) + r16(lh + 28);
    if (e->method == 0) { uint8_t *o = malloc(e->usize + 1); memcpy(o, d, e->usize); *outlen = e->usize; return o; }
    if (e->method == 8) { size_t n; uint8_t *o = inflate_raw(d, e->csize, e->usize, &n); *outlen = n; return o; }
    return NULL;
}

/* --------------------------------------------------------------------- png -- */
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
int png_decode(const uint8_t *d, size_t n, Img *out) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    if (n < 33 || memcmp(d, sig, 8)) return -1;
    size_t p = 8; uint32_t w = 0, h = 0; int depth = 0, ctype = 0, interlace = 0;
    uint8_t pal[256][4]; int npal = 0; uint8_t *idat = NULL; size_t idl = 0; int have_trns_rgb = 0; uint32_t trns_rgb = 0;
    for (int i = 0; i < 256; i++) { pal[i][0] = pal[i][1] = pal[i][2] = 0; pal[i][3] = 255; }
    while (p + 12 <= n) {
        uint32_t len = be32(d + p); const uint8_t *t = d + p + 4, *c = d + p + 8;
        if (p + 12 + len > n) break;
        if (!memcmp(t, "IHDR", 4)) { w = be32(c); h = be32(c + 4); depth = c[8]; ctype = c[9]; interlace = c[12]; }
        else if (!memcmp(t, "PLTE", 4)) { npal = len / 3; for (int i = 0; i < npal && i < 256; i++) { pal[i][0] = c[3*i]; pal[i][1] = c[3*i+1]; pal[i][2] = c[3*i+2]; } }
        else if (!memcmp(t, "tRNS", 4)) {
            if (ctype == 3) for (uint32_t i = 0; i < len && i < 256; i++) pal[i][3] = c[i];
            else if (ctype == 2 && len >= 6) { have_trns_rgb = 1; trns_rgb = (c[1] << 16) | (c[3] << 8) | c[5]; }
        }
        else if (!memcmp(t, "IDAT", 4)) { idat = realloc(idat, idl + len); memcpy(idat + idl, c, len); idl += len; }
        else if (!memcmp(t, "IEND", 4)) break;
        p += 12 + len;
    }
    if (!w || !h || interlace || !idat || idl < 6 || depth > 16) { free(idat); return -2; }
    int ch = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : 4;
    size_t bpp_bits = (size_t)ch * depth, stride = (bpp_bits * w + 7) / 8, bpp = (bpp_bits + 7) / 8;
    size_t rawn; uint8_t *raw = inflate_raw(idat + 2, idl - 2, (stride + 1) * h, &rawn); free(idat);
    if (!raw || rawn < (stride + 1) * h) { free(raw); return -3; }
    uint8_t *prev = calloc(1, stride), *img = malloc(stride * h);
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row = raw + (stride + 1) * y + 1, ft = row[-1], *cur = img + stride * y;
        for (size_t x = 0; x < stride; x++) {
            int a = x >= bpp ? cur[x - bpp] : 0, b = prev[x], c2 = x >= bpp ? prev[x - bpp] : 0, v = row[x];
            switch (ft) {
                case 1: v += a; break; case 2: v += b; break; case 3: v += (a + b) >> 1; break;
                case 4: { int pp = a + b - c2, pa = abs(pp - a), pb = abs(pp - b), pc = abs(pp - c2); v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c2); break; }
            }
            cur[x] = v;
        }
        prev = cur;
    }
    free(raw);
    uint32_t *px = malloc((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *r = img + stride * y;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t R, G, B, A = 255;
            if (depth == 8) {
                switch (ctype) {
                    case 0: R = G = B = r[x]; break;
                    case 2: R = r[3*x]; G = r[3*x+1]; B = r[3*x+2]; if (have_trns_rgb && ((R << 16 | G << 8 | B) == trns_rgb)) A = 0; break;
                    case 3: R = pal[r[x]][0]; G = pal[r[x]][1]; B = pal[r[x]][2]; A = pal[r[x]][3]; break;
                    case 4: R = G = B = r[2*x]; A = r[2*x+1]; break;
                    default: R = r[4*x]; G = r[4*x+1]; B = r[4*x+2]; A = r[4*x+3];
                }
            } else if (depth == 16) {
                switch (ctype) {
                    case 0: R = G = B = r[2*x]; break;
                    case 2: R = r[6*x]; G = r[6*x+2]; B = r[6*x+4]; break;
                    case 4: R = G = B = r[4*x]; A = r[4*x+2]; break;
                    default: R = r[8*x]; G = r[8*x+2]; B = r[8*x+4]; A = r[8*x+6];
                }
            } else {                                   /* 1,2,4 bit gray / palette */
                int per = 8 / depth, sh = 8 - depth * (1 + (x % per)); int v = (r[x / per] >> sh) & ((1 << depth) - 1);
                if (ctype == 3) { R = pal[v][0]; G = pal[v][1]; B = pal[v][2]; A = pal[v][3]; }
                else { R = G = B = v * 255 / ((1 << depth) - 1); }
            }
            px[(size_t)y * w + x] = A << 24 | R << 16 | G << 8 | B;
        }
    }
    free(img); out->w = w; out->h = h; out->px = px; return 0;
}

/* -------------------------------------------------------------------- arsc -- */
typedef struct { uint32_t id; char *s; } RS;
static RS *rs; static int nrs, caprs;
static void rs_add(uint32_t id, char *s) {
    for (int i = 0; i < nrs; i++) if (rs[i].id == id) { free(s); return; }
    if (nrs == caprs) { caprs = caprs ? caprs * 2 : 64; rs = realloc(rs, caprs * sizeof *rs); }
    rs[nrs].id = id; rs[nrs].s = s; nrs++;
}
static char *pool_str(const uint8_t *pool, uint32_t idx) {
    uint32_t cnt = r32(pool + 8), flags = r32(pool + 16), start = r32(pool + 20), hs = r16(pool + 2);
    if (idx >= cnt) return NULL;
    const uint8_t *s = pool + start + r32(pool + hs + 4 * idx);
    if (flags & 0x100) {
        uint32_t cl = *s++; if (cl & 0x80) s++;
        uint32_t bl = *s++; if (bl & 0x80) { bl = ((bl & 0x7f) << 8) | *s++; }
        char *o = malloc(bl + 1); memcpy(o, s, bl); o[bl] = 0; return o;
    } else {
        uint32_t l = r16(s); s += 2; char *o = malloc(l * 3 + 1); size_t n = 0;
        for (uint32_t i = 0; i < l; i++) { uint32_t u = r16(s + 2 * i); if (u < 0x80) o[n++] = u; else if (u < 0x800) { o[n++] = 0xc0 | u >> 6; o[n++] = 0x80 | (u & 0x3f); } else { o[n++] = 0xe0 | u >> 12; o[n++] = 0x80 | ((u >> 6) & 0x3f); o[n++] = 0x80 | (u & 0x3f); } }
        o[n] = 0; return o;
    }
}
int arsc_init(const uint8_t *d, size_t n) {
    if (n < 12 || r16(d) != 0x0002) return -1;
    size_t p = r16(d + 2); const uint8_t *gpool = NULL;
    while (p + 8 <= n) {
        uint32_t type = r16(d + p), hs = r16(d + p + 2), sz = r32(d + p + 4);
        if (type == 0x0001 && !gpool) gpool = d + p;
        else if (type == 0x0200) {
            const uint8_t *pk = d + p; size_t q = hs;
            while (q + 8 <= sz) {
                uint32_t t2 = r16(pk + q), h2 = r16(pk + q + 2), s2 = r32(pk + q + 4);
                if (t2 == 0x0201 && gpool) {
                    uint32_t tid = pk[q + 8], cnt = r32(pk + q + 12), es = r32(pk + q + 16);
                    for (uint32_t i = 0; i < cnt; i++) {
                        uint32_t off = r32(pk + q + h2 + 4 * i); if (off == 0xffffffffu) continue;
                        const uint8_t *e = pk + q + es + off;
                        if (r16(e + 2) & 1) continue;                    /* complex entry */
                        const uint8_t *v = e + r16(e);
                        if (v[3] == 0x03) { char *s = pool_str(gpool, r32(v + 4)); if (s) rs_add(0x7f000000u | tid << 16 | i, s); }
                    }
                }
                if (!s2) break; q += s2;
            }
        }
        if (!sz) break; p += sz;
    }
    return 0;
}
const char *arsc_get(uint32_t id) { for (int i = 0; i < nrs; i++) if (rs[i].id == id) return rs[i].s; return NULL; }
