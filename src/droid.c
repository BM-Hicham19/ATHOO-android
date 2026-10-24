/* android.* natives: activity glue, software canvas, input */
#include "plat.h"
#include "res.h"
#include "audio.h"
#include <math.h>

static Class *C(const char *n) { return vm_class(n); }
static Obj *jstr(const char *s) { return vm_new_string_utf8(s); }
int plat_log = 0;

/* ------------------------------------------------------------ singletons -- */
static Obj *singleton(const char *cls, Obj **slot) {
    if (!*slot) { *slot = vm_new_obj(C(cls)); vm_add_root(slot); }
    return *slot;
}
static Obj *g_res, *g_assets, *g_prefs, *g_vib, *g_pm, *g_win, *g_wm, *g_disp, *g_cb;
static Obj *g_view, *g_thread, *g_act;
static Obj *g_screen_bmp, *g_screen_cv;
static Obj *hq[64]; static int nhq;

/* ------------------------------------------------------------------ gfx --- */
typedef struct { float a, b, c, d, tx, ty; } Mx;
typedef struct { int w, h; uint32_t *px; } Bm;
typedef struct { Mx m; int x0, y0, x1, y1; } CS;
typedef struct { CS cur; CS st[32]; int n; } CV;
typedef struct { int n; uint8_t mv[512]; float x[512], y[512]; } PP;
typedef struct { uint32_t color; int style; float stroke, textsize; int align; } PT;

static Mx mm_id(void) { Mx m = {1, 0, 0, 1, 0, 0}; return m; }
static Mx mm_mul(Mx A, Mx B) {
    Mx r; r.a = A.a * B.a + A.c * B.b; r.b = A.b * B.a + A.d * B.b; r.c = A.a * B.c + A.c * B.d; r.d = A.b * B.c + A.d * B.d;
    r.tx = A.a * B.tx + A.c * B.ty + A.tx; r.ty = A.b * B.tx + A.d * B.ty + A.ty; return r;
}
static Mx mm_trans(float x, float y) { Mx m = {1, 0, 0, 1, x, y}; return m; }
static Mx mm_scale(float x, float y) { Mx m = {x, 0, 0, y, 0, 0}; return m; }
static void mm_pt(const Mx *m, float x, float y, float *ox, float *oy) { *ox = m->a * x + m->c * y + m->tx; *oy = m->b * x + m->d * y + m->ty; }

static Bm bm_of(Obj *o) { Bm b; b.w = o->f[0].i; b.h = o->f[1].i; b.px = (uint32_t *)o->data; return b; }
static Obj *new_bitmap(int w, int h, int cfg) {
    if (w < 1) w = 1; if (h < 1) h = 1;
    Obj *o = vm_new_obj(C("Landroid/graphics/Bitmap;"));
    o->data = calloc((size_t)w * h, 4); if (!o->data) { vm_throw("Ljava/lang/OutOfMemoryError;", "bitmap"); return NULL; }
    o->cap = (int32_t)((size_t)w * h * 4); vm_note_alloc((size_t)w * h * 4);
    SETI_(o->f[0], w); SETI_(o->f[1], h); SETI_(o->f[2], cfg); SETI_(o->f[3], 1);
    if (cfg == 1) { uint32_t *p = o->data; for (size_t i = 0; i < (size_t)w * h; i++) p[i] = 0xff000000u; }
    return o;
}
static CV *cv_of(Obj *c) { return (CV *)c->data; }
static Bm cv_bm(Obj *c) { return bm_of(c->f[0].o); }

static inline uint32_t div255(uint32_t x) { x += 128; return (x + (x >> 8)) >> 8; }
static inline void blend_px(uint32_t *dp, uint32_t s, uint32_t ga) {
    uint32_t sa = s >> 24; if (ga != 255) sa = div255(sa * ga);
    if (!sa) return;
    uint32_t d = *dp, da = d >> 24;
    if (sa == 255 || da == 0) { *dp = (sa << 24) | (s & 0xffffff); return; }
    if (da == 255) {
        uint32_t ia = 255 - sa;
        uint32_t r = div255(((s >> 16) & 255) * sa + ((d >> 16) & 255) * ia), g = div255(((s >> 8) & 255) * sa + ((d >> 8) & 255) * ia), b = div255((s & 255) * sa + (d & 255) * ia);
        *dp = 0xff000000u | r << 16 | g << 8 | b; return;
    }
    uint32_t oa = sa + div255(da * (255 - sa));
    uint32_t wd = div255(da * (255 - sa));
    uint32_t r = (((s >> 16) & 255) * sa + ((d >> 16) & 255) * wd) / oa, g = (((s >> 8) & 255) * sa + ((d >> 8) & 255) * wd) / oa, b = ((s & 255) * sa + (d & 255) * wd) / oa;
    *dp = oa << 24 | r << 16 | g << 8 | b;
}
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

static int *g_tab; static int g_tabn;
static int *tab(int n) { if (n > g_tabn) { g_tabn = n + 64; g_tab = realloc(g_tab, 2 * g_tabn * sizeof(int)); } return g_tab; }

/* draw src (transformed by M into device space) clipped to cs clip */
static void blit(Bm d, const CS *cs, Bm s, Mx M, uint32_t galpha) {
    float xs[4], ys[4]; mm_pt(&M, 0, 0, &xs[0], &ys[0]); mm_pt(&M, s.w, 0, &xs[1], &ys[1]); mm_pt(&M, s.w, s.h, &xs[2], &ys[2]); mm_pt(&M, 0, s.h, &xs[3], &ys[3]);
    float mnx = xs[0], mxx = xs[0], mny = ys[0], mxy = ys[0];
    for (int i = 1; i < 4; i++) { if (xs[i] < mnx) mnx = xs[i]; if (xs[i] > mxx) mxx = xs[i]; if (ys[i] < mny) mny = ys[i]; if (ys[i] > mxy) mxy = ys[i]; }
    int x0 = imax(cs->x0, imax(0, (int)floorf(mnx))), x1 = imin(cs->x1, imin(d.w, (int)ceilf(mxx)));
    int y0 = imax(cs->y0, imax(0, (int)floorf(mny))), y1 = imin(cs->y1, imin(d.h, (int)ceilf(mxy)));
    if (x0 >= x1 || y0 >= y1 || !galpha) return;
    if (M.b == 0 && M.c == 0 && M.a != 0 && M.d != 0) {
        int *tx = tab(x1 - x0 + (y1 - y0)), *ty = tx + (x1 - x0);
        for (int x = x0; x < x1; x++) { int u = (int)floorf((x + 0.5f - M.tx) / M.a); tx[x - x0] = (u >= 0 && u < s.w) ? u : -1; }
        for (int y = y0; y < y1; y++) { int v = (int)floorf((y + 0.5f - M.ty) / M.d); ty[y - y0] = (v >= 0 && v < s.h) ? v : -1; }
        for (int y = y0; y < y1; y++) {
            int v = ty[y - y0]; if (v < 0) continue;
            const uint32_t *sr = s.px + (size_t)v * s.w; uint32_t *dr = d.px + (size_t)y * d.w;
            if (galpha == 255) {
                for (int x = x0; x < x1; x++) { int u = tx[x - x0]; if (u < 0) continue; uint32_t c = sr[u]; uint32_t a = c >> 24; if (a == 255) dr[x] = c; else if (a) blend_px(&dr[x], c, 255); }
            } else for (int x = x0; x < x1; x++) { int u = tx[x - x0]; if (u >= 0) blend_px(&dr[x], sr[u], galpha); }
        }
        return;
    }
    float det = M.a * M.d - M.b * M.c; if (fabsf(det) < 1e-9f) return;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
        float dx = x + 0.5f - M.tx, dy = y + 0.5f - M.ty;
        int u = (int)floorf((M.d * dx - M.c * dy) / det), v = (int)floorf((-M.b * dx + M.a * dy) / det);
        if (u >= 0 && u < s.w && v >= 0 && v < s.h) blend_px(&d.px[(size_t)y * d.w + x], s.px[(size_t)v * s.w + u], galpha);
    }
}
static void fill_dev_rect(Bm d, const CS *cs, int x0, int y0, int x1, int y1, uint32_t color) {
    x0 = imax(x0, imax(0, cs->x0)); y0 = imax(y0, imax(0, cs->y0)); x1 = imin(x1, imin(d.w, cs->x1)); y1 = imin(y1, imin(d.h, cs->y1));
    uint32_t a = color >> 24; if (!a) return;
    for (int y = y0; y < y1; y++) { uint32_t *r = d.px + (size_t)y * d.w; if (a == 255) for (int x = x0; x < x1; x++) r[x] = color; else for (int x = x0; x < x1; x++) blend_px(&r[x], color, 255); }
}
static int cmpf(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return x < y ? -1 : x > y; }
static void fill_poly(Bm d, const CS *cs, const float *p, int n, uint32_t color) {
    if (n < 3) return;
    float miny = p[1], maxy = p[1]; for (int i = 1; i < n; i++) { if (p[2*i+1] < miny) miny = p[2*i+1]; if (p[2*i+1] > maxy) maxy = p[2*i+1]; }
    int y0 = imax(imax(0, cs->y0), (int)floorf(miny)), y1 = imin(imin(d.h, cs->y1), (int)ceilf(maxy));
    float *xs = malloc(sizeof(float) * (n + 2));
    for (int y = y0; y < y1; y++) {
        float yc = y + 0.5f; int k = 0;
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n; float ax = p[2*i], ay = p[2*i+1], bx = p[2*j], by = p[2*j+1];
            if ((ay <= yc && by > yc) || (by <= yc && ay > yc)) xs[k++] = ax + (yc - ay) * (bx - ax) / (by - ay);
        }
        qsort(xs, k, sizeof(float), cmpf);
        for (int i = 0; i + 1 < k; i += 2) fill_dev_rect(d, cs, (int)ceilf(xs[i] - 0.5f), y, (int)ceilf(xs[i+1] - 0.5f), y + 1, color);
    }
    free(xs);
}
static float cs_scale(const CS *cs) { float s = sqrtf(fabsf(cs->m.a * cs->m.d - cs->m.b * cs->m.c)); return s > 0 ? s : 1; }
static void line_dev(Bm d, const CS *cs, float x0, float y0, float x1, float y1, float w, uint32_t color) {
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
    if (w < 1) w = 1;
    if (len < 0.001f) { fill_dev_rect(d, cs, (int)floorf(x0 - w / 2 + 0.5f), (int)floorf(y0 - w / 2 + 0.5f), (int)floorf(x0 + w / 2 + 0.5f) + 1, (int)floorf(y0 + w / 2 + 0.5f) + 1, color); return; }
    float nx = -dy / len * w / 2, ny = dx / len * w / 2;
    float q[8] = {x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny};
    fill_poly(d, cs, q, 4, color);
}
static void draw_polyline_local(Obj *cv, const float *pts, int n, int closed, PT *pt) {
    Bm d = cv_bm(cv); CV *c = cv_of(cv); float w = (pt->stroke > 0 ? pt->stroke : 1) * cs_scale(&c->cur);
    for (int i = 0; i + 1 < n || (closed && i < n); i++) {
        int j = (i + 1) % n; float ax, ay, bx, by;
        mm_pt(&c->cur.m, pts[2*i], pts[2*i+1], &ax, &ay); mm_pt(&c->cur.m, pts[2*j], pts[2*j+1], &bx, &by);
        line_dev(d, &c->cur, ax, ay, bx, by, w, pt->color);
    }
}
static void draw_poly_local(Obj *cv, const float *pts, int n, int closed, PT *pt) {
    if (pt->style == 1) { draw_polyline_local(cv, pts, n, closed, pt); return; }
    CV *c = cv_of(cv); float *dv = malloc(sizeof(float) * 2 * n);
    for (int i = 0; i < n; i++) mm_pt(&c->cur.m, pts[2*i], pts[2*i+1], &dv[2*i], &dv[2*i+1]);
    fill_poly(cv_bm(cv), &c->cur, dv, n, pt->color); free(dv);
}

/* ----------------------------------------------------------------- natives -- */
#define PTO(o) ((PT *)(o)->data)
NATIVE(n_nop) { (void)args; (void)ret; }
NATIVE(n_ret0) { (void)args; RETI(0); }
NATIVE(n_ret_this) { RETO(args[0].o); }
NATIVE(n_ret_null) { (void)args; RETO(NULL); }

/* Bitmap */
NATIVE(n_bm_createWHC) { int cfg = args[2].o && args[2].o == vm_native_static("Landroid/graphics/Bitmap$Config;.RGB_565")->o ? 1 : 0; Obj *b = new_bitmap(args[0].i, args[1].i, cfg); if (b) RETO(b); }
NATIVE(n_bm_createSub) {
    Obj *s = args[0].o; if (!s) { vm_throw("Ljava/lang/NullPointerException;", "createBitmap"); return; }
    int x = args[1].i, y = args[2].i, w = args[3].i, h = args[4].i; Bm sb = bm_of(s);
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > sb.w || y + h > sb.h) { vm_throw("Ljava/lang/IllegalArgumentException;", "createBitmap sub"); return; }
    Obj *b = new_bitmap(w, h, s->f[2].i); if (!b) return;
    for (int r = 0; r < h; r++) memcpy((uint32_t *)b->data + (size_t)r * w, sb.px + (size_t)(y + r) * sb.w + x, 4 * w);
    RETO(b);
}
static Mx *mx_of(Obj *m) { return (Mx *)m->data; }
NATIVE(n_bm_createMat) {
    Obj *s = args[0].o; if (!s) { vm_throw("Ljava/lang/NullPointerException;", "createBitmap"); return; }
    int x = args[1].i, y = args[2].i, w = args[3].i, h = args[4].i; Obj *mo = args[5].o; Bm sb = bm_of(s);
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > sb.w || y + h > sb.h) { vm_throw("Ljava/lang/IllegalArgumentException;", "createBitmap xform"); return; }
    Mx M = mo ? *mx_of(mo) : mm_id();
    float xs[4], ys[4]; mm_pt(&M, 0, 0, &xs[0], &ys[0]); mm_pt(&M, w, 0, &xs[1], &ys[1]); mm_pt(&M, w, h, &xs[2], &ys[2]); mm_pt(&M, 0, h, &xs[3], &ys[3]);
    float mnx = xs[0], mxx = xs[0], mny = ys[0], mxy = ys[0];
    for (int i = 1; i < 4; i++) { if (xs[i] < mnx) mnx = xs[i]; if (xs[i] > mxx) mxx = xs[i]; if (ys[i] < mny) mny = ys[i]; if (ys[i] > mxy) mxy = ys[i]; }
    int ow = (int)ceilf(mxx - mnx - 0.001f), oh = (int)ceilf(mxy - mny - 0.001f);
    Obj *b = new_bitmap(ow, oh, s->f[2].i); if (!b) return;
    if (s->f[2].i == 1) memset(b->data, 0, (size_t)b->f[0].i * b->f[1].i * 4);
    Bm db = bm_of(b); CS cs; cs.m = mm_id(); cs.x0 = 0; cs.y0 = 0; cs.x1 = db.w; cs.y1 = db.h;
    Bm sub; sub.w = w; sub.h = h; sub.px = malloc((size_t)w * h * 4);
    for (int r = 0; r < h; r++) memcpy(sub.px + (size_t)r * w, sb.px + (size_t)(y + r) * sb.w + x, 4 * w);
    Mx T = mm_mul(mm_trans(-mnx, -mny), M);
    for (size_t i = 0; i < (size_t)db.w * db.h; i++) db.px[i] = 0;
    blit(db, &cs, sub, T, 255); free(sub.px);
    RETO(b);
}
NATIVE(n_bm_createPix) {
    Obj *a = args[0].o; int w = args[1].i, h = args[2].i; if (!a || (int64_t)w * h > a->len) { vm_throw("Ljava/lang/IllegalArgumentException;", "createBitmap pixels"); return; }
    Obj *b = new_bitmap(w, h, 0); if (!b) return; memcpy(b->data, a->data, (size_t)w * h * 4); RETO(b);
}
NATIVE(n_bm_copy) { Obj *s = args[0].o; Bm sb = bm_of(s); Obj *b = new_bitmap(sb.w, sb.h, s->f[2].i); if (!b) return; memcpy(b->data, sb.px, (size_t)sb.w * sb.h * 4); SETI_(b->f[3], args[2].i); RETO(b); }
NATIVE(n_bm_getConfig) { RETO(vm_native_static(args[0].o->f[2].i == 1 ? "Landroid/graphics/Bitmap$Config;.RGB_565" : "Landroid/graphics/Bitmap$Config;.ARGB_8888")->o); }
NATIVE(n_bm_getW) { RETI(args[0].o->f[0].i); }
NATIVE(n_bm_getH) { RETI(args[0].o->f[1].i); }
NATIVE(n_bm_mutable) { RETI(args[0].o->f[3].i); }
NATIVE(n_bm_getPixels) {
    Bm b = bm_of(args[0].o); Obj *a = args[1].o; int off = args[2].i, stride = args[3].i, x = args[4].i, y = args[5].i, w = args[6].i, h = args[7].i; (void)ret;
    if (!a || x < 0 || y < 0 || x + w > b.w || y + h > b.h || off < 0 || (int64_t)off + (int64_t)(h - 1) * stride + w > a->len) { vm_throw("Ljava/lang/ArrayIndexOutOfBoundsException;", "getPixels"); return; }
    for (int r = 0; r < h; r++) memcpy(ARR_I(a) + off + (size_t)r * stride, b.px + (size_t)(y + r) * b.w + x, 4 * w);
}
NATIVE(n_bmf_decode) {
    Obj *a = args[0].o; int off = args[1].i, n = args[2].i; if (!a || off < 0 || n < 0 || off + n > a->len) { RETO(NULL); return; }
    Img im; if (png_decode(ARR_U8(a) + off, n, &im)) { RETO(NULL); return; }
    Obj *b = new_bitmap(im.w, im.h, 0); if (!b) { free(im.px); return; }
    memcpy(b->data, im.px, (size_t)im.w * im.h * 4); free(im.px); SETI_(b->f[3], 0); RETO(b);
}
NATIVE(n_cfg_equals) { RETI(args[0].o == args[1].o); }

/* Matrix */
NATIVE(n_mx_init) { (void)ret; Obj *o = args[0].o; o->data = malloc(sizeof(Mx)); *mx_of(o) = mm_id(); }
NATIVE(n_mx_setScale) { (void)ret; *mx_of(args[0].o) = mm_scale(args[1].f, args[2].f); }
NATIVE(n_mx_postRotate) { Mx *m = mx_of(args[0].o); float r = args[1].f * 3.14159265358979f / 180; Mx R = {cosf(r), sinf(r), -sinf(r), cosf(r), 0, 0}; *m = mm_mul(R, *m); RETI(1); }
NATIVE(n_mx_postTranslate) { Mx *m = mx_of(args[0].o); *m = mm_mul(mm_trans(args[1].f, args[2].f), *m); RETI(1); }
NATIVE(n_mx_preTranslate) { Mx *m = mx_of(args[0].o); *m = mm_mul(*m, mm_trans(args[1].f, args[2].f)); RETI(1); }

/* Paint */
NATIVE(n_pt_init) { (void)ret; Obj *o = args[0].o; PT *p = calloc(1, sizeof(PT)); p->color = 0xff000000u; p->textsize = 12; o->data = p; }
NATIVE(n_pt_getColor) { RETI(PTO(args[0].o)->color); }
NATIVE(n_pt_setColor) { (void)ret; PTO(args[0].o)->color = args[1].u; }
NATIVE(n_pt_setStroke) { (void)ret; PTO(args[0].o)->stroke = args[1].f; }
NATIVE(n_pt_setTextSize) { (void)ret; PTO(args[0].o)->textsize = args[1].f; }
NATIVE(n_pt_setStyle) { (void)ret; PTO(args[0].o)->style = args[1].o == vm_native_static("Landroid/graphics/Paint$Style;.STROKE")->o; }
NATIVE(n_pt_setAlign) { (void)ret; PTO(args[0].o)->align = 0; }
NATIVE(n_pt_getTextBounds) {
    (void)ret; Obj *r = args[4].o; if (!r) return; PT *p = PTO(args[0].o);
    int n = args[3].i - args[2].i; if (n < 0) n = 0;
    SETI_(r->f[0], 0); SETI_(r->f[1], -(int)p->textsize); SETI_(r->f[2], (int)(n * p->textsize * 0.5f)); SETI_(r->f[3], 0);
}
NATIVE(n_pt_getTextBoundsC) {
    (void)ret; Obj *r = args[4].o; if (!r) return; PT *p = PTO(args[0].o);
    int n = args[3].i; if (n < 0) n = 0;
    SETI_(r->f[0], 0); SETI_(r->f[1], -(int)p->textsize); SETI_(r->f[2], (int)(n * p->textsize * 0.5f)); SETI_(r->f[3], 0);
}
NATIVE(n_ret_arg1) { RETO(args[1].o); }
NATIVE(n_color_rgb) { RETI(0xff000000u | (args[0].i & 255) << 16 | (args[1].i & 255) << 8 | (args[2].i & 255)); }

/* Path / Rect */
NATIVE(n_path_init) { (void)ret; args[0].o->data = calloc(1, sizeof(PP)); }
NATIVE(n_path_moveTo) { (void)ret; PP *p = args[0].o->data; if (p->n < 512) { p->mv[p->n] = 1; p->x[p->n] = args[1].f; p->y[p->n] = args[2].f; p->n++; } }
NATIVE(n_path_lineTo) { (void)ret; PP *p = args[0].o->data; if (p->n < 512) { p->mv[p->n] = 0; p->x[p->n] = args[1].f; p->y[p->n] = args[2].f; p->n++; } }
NATIVE(n_rect_width) { RETI(args[0].o->f[2].i - args[0].o->f[0].i); }
NATIVE(n_rect_height) { RETI(args[0].o->f[3].i - args[0].o->f[1].i); }
NATIVE(n_rectf_init4) { (void)ret; for (int i = 0; i < 4; i++) { args[0].o->f[i].j = 0; args[0].o->f[i].f = args[1 + i].f; } }
NATIVE(n_tf_default) { (void)args; RETO(vm_new_obj(C("Landroid/graphics/Typeface;"))); }

/* Canvas */
NATIVE(n_cv_init) {
    (void)ret; Obj *o = args[0].o, *b = args[1].o; if (!b) { vm_throw("Ljava/lang/NullPointerException;", "Canvas(null)"); return; }
    CV *c = calloc(1, sizeof(CV)); o->data = c; o->f[0].o = b;
    c->cur.m = mm_id(); c->cur.x0 = 0; c->cur.y0 = 0; c->cur.x1 = b->f[0].i; c->cur.y1 = b->f[1].i;
}
static void clip_intersect(CS *cs, int x0, int y0, int x1, int y1) { cs->x0 = imax(cs->x0, x0); cs->y0 = imax(cs->y0, y0); cs->x1 = imin(cs->x1, x1); cs->y1 = imin(cs->y1, y1); }
static void dev_rect(const Mx *m, float l, float t, float r, float b, float *x0, float *y0, float *x1, float *y1) {
    float ax, ay, bx, by; mm_pt(m, l, t, &ax, &ay); mm_pt(m, r, b, &bx, &by);
    *x0 = fminf(ax, bx); *x1 = fmaxf(ax, bx); *y0 = fminf(ay, by); *y1 = fmaxf(ay, by);
}
NATIVE(n_cv_clipRectI) { CV *c = cv_of(args[0].o); float x0, y0, x1, y1; dev_rect(&c->cur.m, args[1].i, args[2].i, args[3].i, args[4].i, &x0, &y0, &x1, &y1);
    clip_intersect(&c->cur, (int)floorf(x0 + 0.5f), (int)floorf(y0 + 0.5f), (int)floorf(x1 + 0.5f), (int)floorf(y1 + 0.5f)); RETI(c->cur.x0 < c->cur.x1 && c->cur.y0 < c->cur.y1); }
NATIVE(n_cv_clipRectF) {
    CV *c = cv_of(args[0].o); float x0, y0, x1, y1; dev_rect(&c->cur.m, args[1].f, args[2].f, args[3].f, args[4].f, &x0, &y0, &x1, &y1);
    int a = (int)floorf(x0 + 0.5f), b = (int)floorf(y0 + 0.5f), cc = (int)floorf(x1 + 0.5f), d = (int)floorf(y1 + 0.5f);
    Bm bm = cv_bm(args[0].o);
    if (args[5].o && args[5].o == vm_native_static("Landroid/graphics/Region$Op;.REPLACE")->o) { c->cur.x0 = imax(0, a); c->cur.y0 = imax(0, b); c->cur.x1 = imin(bm.w, cc); c->cur.y1 = imin(bm.h, d); }
    else clip_intersect(&c->cur, a, b, cc, d);
    RETI(c->cur.x0 < c->cur.x1 && c->cur.y0 < c->cur.y1);
}
NATIVE(n_cv_getClipBounds) {
    CV *c = cv_of(args[0].o); Obj *r = vm_new_obj(C("Landroid/graphics/Rect;")); Mx *m = &c->cur.m;
    float x0 = (c->cur.x0 - m->tx) / (m->a ? m->a : 1), x1 = (c->cur.x1 - m->tx) / (m->a ? m->a : 1), y0 = (c->cur.y0 - m->ty) / (m->d ? m->d : 1), y1 = (c->cur.y1 - m->ty) / (m->d ? m->d : 1);
    SETI_(r->f[0], (int)floorf(fminf(x0, x1) + 0.001f)); SETI_(r->f[1], (int)floorf(fminf(y0, y1) + 0.001f)); SETI_(r->f[2], (int)ceilf(fmaxf(x0, x1) - 0.001f)); SETI_(r->f[3], (int)ceilf(fmaxf(y0, y1) - 0.001f));
    if (getenv("AVM_CLIPDBG")) fprintf(stderr, "[clip] isscreen=%d cls=%s nf=%d data=%p dev %d,%d,%d,%d m=%g,%g -> %d,%d,%d,%d\n", args[0].o == g_screen_cv, args[0].o->cls->name, args[0].o->nf, args[0].o->data, c->cur.x0, c->cur.y0, c->cur.x1, c->cur.y1, m->a, m->tx, r->f[0].i, r->f[1].i, r->f[2].i, r->f[3].i);
    RETO(r);
}
NATIVE(n_cv_save) { CV *c = cv_of(args[0].o); if (c->n < 32) c->st[c->n++] = c->cur; RETI(c->n); }
NATIVE(n_cv_restore) { (void)ret; CV *c = cv_of(args[0].o); if (c->n > 0) c->cur = c->st[--c->n]; }
NATIVE(n_cv_scale) { (void)ret; CV *c = cv_of(args[0].o); if (getenv("AVM_CLIPDBG")) fprintf(stderr, "[scale] %g %g\n", args[1].f, args[2].f); c->cur.m = mm_mul(c->cur.m, mm_scale(args[1].f, args[2].f)); }
NATIVE(n_cv_drawBitmap) {
    (void)ret; Obj *cvo = args[0].o, *bo = args[1].o; if (!bo) { vm_throw("Ljava/lang/NullPointerException;", "drawBitmap"); return; }
    CV *c = cv_of(cvo); uint32_t ga = args[4].o ? PTO(args[4].o)->color >> 24 : 255;
    blit(cv_bm(cvo), &c->cur, bm_of(bo), mm_mul(c->cur.m, mm_trans(args[2].f, args[3].f)), ga);
}
NATIVE(n_cv_drawBitmapM) {
    (void)ret; Obj *cvo = args[0].o, *bo = args[1].o; if (!bo) { vm_throw("Ljava/lang/NullPointerException;", "drawBitmap"); return; }
    CV *c = cv_of(cvo); uint32_t ga = args[3].o ? PTO(args[3].o)->color >> 24 : 255;
    blit(cv_bm(cvo), &c->cur, bm_of(bo), mm_mul(c->cur.m, args[2].o ? *mx_of(args[2].o) : mm_id()), ga);
}
NATIVE(n_cv_drawBitmapI) {
    (void)ret; Obj *cvo = args[0].o, *a = args[1].o; int off = args[2].i, stride = args[3].i, x = args[4].i, y = args[5].i, w = args[6].i, h = args[7].i, alpha = args[8].i;
    if (!a || w <= 0 || h <= 0 || off < 0 || (int64_t)off + (int64_t)(h - 1) * stride + w > a->len) return;
    Bm s; s.w = w; s.h = h; s.px = malloc((size_t)w * h * 4);
    for (int r = 0; r < h; r++) for (int q = 0; q < w; q++) { uint32_t p = (uint32_t)ARR_I(a)[off + (size_t)r * stride + q]; s.px[(size_t)r * w + q] = alpha ? p : (p | 0xff000000u); }
    CV *c = cv_of(cvo); blit(cv_bm(cvo), &c->cur, s, mm_mul(c->cur.m, mm_trans((float)x, (float)y)), args[9].o ? PTO(args[9].o)->color >> 24 : 255); free(s.px);
}
NATIVE(n_cv_drawRect) {
    (void)ret; Obj *cvo = args[0].o; CV *c = cv_of(cvo); PT *p = PTO(args[5].o); Bm d = cv_bm(cvo);
    float l = args[1].f, t = args[2].f, r = args[3].f, b = args[4].f;
    if (p->style == 1) { float pts[8] = {l, t, r, t, r, b, l, b}; draw_polyline_local(cvo, pts, 4, 1, p); return; }
    if (c->cur.m.b == 0 && c->cur.m.c == 0) {
        float x0, y0, x1, y1; dev_rect(&c->cur.m, l, t, r, b, &x0, &y0, &x1, &y1);
        fill_dev_rect(d, &c->cur, (int)floorf(x0 + 0.5f), (int)floorf(y0 + 0.5f), (int)floorf(x1 + 0.5f), (int)floorf(y1 + 0.5f), p->color);
    } else { float pts[8] = {l, t, r, t, r, b, l, b}; draw_poly_local(cvo, pts, 4, 1, p); }
}
NATIVE(n_cv_drawLine) {
    (void)ret; Obj *cvo = args[0].o; CV *c = cv_of(cvo); PT *p = PTO(args[5].o); float ax, ay, bx, by;
    mm_pt(&c->cur.m, args[1].f, args[2].f, &ax, &ay); mm_pt(&c->cur.m, args[3].f, args[4].f, &bx, &by);
    line_dev(cv_bm(cvo), &c->cur, ax, ay, bx, by, (p->stroke > 0 ? p->stroke : 1) * cs_scale(&c->cur), p->color);
}
NATIVE(n_cv_drawArc) {
    (void)ret; Obj *cvo = args[0].o, *r = args[1].o; PT *p = PTO(args[5].o); if (!r) return;
    float l = r->f[0].f, t = r->f[1].f, rr = r->f[2].f, b = r->f[3].f, cx = (l + rr) / 2, cy = (t + b) / 2, rx = (rr - l) / 2, ry = (b - t) / 2;
    float st = args[2].f, sw = args[3].f; int center = args[4].i;
    if (sw > 360) sw = 360; if (sw < -360) sw = -360;
    int seg = (int)(fabsf(sw) / 4) + 4; float pts[2 * 120 + 4]; int n = 0; if (seg > 118) seg = 118;
    if (center && p->style == 0) { pts[n++] = cx; pts[n++] = cy; }
    for (int i = 0; i <= seg; i++) { float a = (st + sw * i / seg) * 3.14159265358979f / 180; pts[n++] = cx + rx * cosf(a); pts[n++] = cy + ry * sinf(a); }
    draw_poly_local(cvo, pts, n / 2, p->style == 0, p);
}
NATIVE(n_cv_drawRoundRect) {
    (void)ret; Obj *cvo = args[0].o, *r = args[1].o; PT *p = PTO(args[4].o); if (!r) return;
    float l = r->f[0].f, t = r->f[1].f, rr = r->f[2].f, b = r->f[3].f, rx = fminf(args[2].f, (rr - l) / 2), ry = fminf(args[3].f, (b - t) / 2);
    float pts[2 * 40]; int n = 0; const int S = 8;
    float cx[4] = {rr - rx, l + rx, l + rx, rr - rx}, cy[4] = {t + ry, t + ry, b - ry, b - ry}; float a0[4] = {270, 180, 90, 0};
    for (int k = 0; k < 4; k++) for (int i = 0; i <= S; i++) { float a = (a0[k] + 90.0f * i / S) * 3.14159265358979f / 180; pts[n++] = cx[k] + rx * cosf(a); pts[n++] = cy[k] + ry * sinf(a); }
    draw_poly_local(cvo, pts, n / 2, 1, p);
}
NATIVE(n_cv_drawPath) {
    (void)ret; Obj *cvo = args[0].o; PP *pp = args[1].o ? args[1].o->data : NULL; PT *p = PTO(args[2].o); if (!pp || pp->n < 2) return;
    float *pts = malloc(sizeof(float) * 2 * pp->n);
    if (p->style == 1) {
        for (int i = 1; i < pp->n; i++) if (!pp->mv[i]) { float seg[4] = {pp->x[i-1], pp->y[i-1], pp->x[i], pp->y[i]}; draw_polyline_local(cvo, seg, 2, 0, p); }
    } else { for (int i = 0; i < pp->n; i++) { pts[2*i] = pp->x[i]; pts[2*i+1] = pp->y[i]; } draw_poly_local(cvo, pts, pp->n, 1, p); }
    free(pts);
}

/* ------------------------------------------------------------ views/input -- */
static Obj *mk_enum_bmcfg_a(void) { return vm_new_obj(C("Landroid/graphics/Bitmap$Config;")); }
static Obj *mk_enum_generic_obj(void) { return vm_new_obj(C("Ljava/lang/Object;")); }
NATIVE(n_sv_init) { (void)ret; Obj *v = args[0].o; v->f[0].o = vm_new_obj(C("Landroid/view/SurfaceHolder;")); }
NATIVE(n_sv_getHolder) { RETO(args[0].o->f[0].o); }
NATIVE(n_sv_setKey) { (void)ret; args[0].o->f[1].o = args[1].o; }
NATIVE(n_sv_setTouch) { (void)ret; args[0].o->f[2].o = args[1].o; }
NATIVE(n_sh_addCallback) { (void)ret; args[0].o->f[0].o = args[1].o; }
static void screen_reset(void) {
    CV *c = cv_of(g_screen_cv); c->n = 0; c->cur.m = mm_scale((float)SCR_W / DISP_W, (float)SCR_H / DISP_H);
    c->cur.x0 = 0; c->cur.y0 = 0; c->cur.x1 = SCR_W; c->cur.y1 = SCR_H;
}
NATIVE(n_sh_lock) { (void)args; screen_reset(); RETO(g_screen_cv); }
NATIVE(n_sh_unlock) { (void)args; (void)ret; plat_present((const uint32_t *)g_screen_bmp->data, SCR_W, SCR_H); app_frame_hook(1); }
NATIVE(n_disp_w) { (void)args; if (getenv("AVM_CLIPDBG")) fprintf(stderr, "[disp] w\n"); RETI(DISP_W); }
NATIVE(n_disp_h) { (void)args; RETI(DISP_H); }
NATIVE(n_wm_getDisplay) { RETO(singleton("Landroid/view/Display;", &g_disp)); (void)args; }
NATIVE(n_act_getWindow) { (void)args; RETO(singleton("Landroid/view/Window;", &g_win)); }
NATIVE(n_act_getWM) { (void)args; RETO(singleton("Landroid/view/WindowManager;", &g_wm)); }
NATIVE(n_act_getRes) { (void)args; RETO(singleton("Landroid/content/res/Resources;", &g_res)); }
NATIVE(n_act_getAssets) { (void)args; RETO(singleton("Landroid/content/res/AssetManager;", &g_assets)); }
NATIVE(n_act_getPrefs) { (void)args; RETO(singleton("Landroid/content/SharedPreferences;", &g_prefs)); }
NATIVE(n_act_getPM) { (void)args; RETO(singleton("Landroid/content/pm/PackageManager;", &g_pm)); }
NATIVE(n_act_getSys) { (void)args; RETO(singleton("Landroid/os/Vibrator;", &g_vib)); }
NATIVE(n_act_pkg) { (void)args; RETO(jstr("com.globalfun.adventuretime.free")); }
NATIVE(n_act_setContent) { (void)ret; g_view = args[1].o; vm_add_root(&g_view); }
NATIVE(n_act_finish) { (void)args; (void)ret; plat_quit(0); }
NATIVE(n_act_reqFeature) { (void)args; RETI(1); }
NATIVE(n_pm_getInfo) { Obj *o = vm_new_obj(C("Landroid/content/pm/PackageInfo;")); o->f[0].o = jstr("1.0.8"); SETI_(o->f[1], 8); (void)args; RETO(o); }
NATIVE(n_prefs_getString) { RETO(args[2].o); }
NATIVE(n_res_getString) { const char *s = arsc_get(args[1].u); RETO(s ? jstr(s) : jstr("")); }
NATIVE(n_assets_open) {
    Obj *nm = args[1].o; if (!nm) { vm_throw("Ljava/lang/NullPointerException;", "AssetManager.open"); return; }
    char *u = vm_string_to_utf8(nm); char path[600]; snprintf(path, sizeof path, "assets/%s", u);
    size_t n; uint8_t *d = zip_extract(path, &n);
    if (!d) { vm_throw("Ljava/io/FileNotFoundException;", u); free(u); return; }
    free(u); Obj *o = jlib_new_instream(d, n); free(d); RETO(o);
}
NATIVE(n_post) { if (nhq < 64 && args[1].o) hq[nhq++] = args[1].o; RETI(1); }
NATIVE(n_runOnUi) { (void)ret; if (nhq < 64 && args[1].o) hq[nhq++] = args[1].o; }
NATIVE(n_log_d) { if (plat_log) { char *a = vm_string_to_utf8(args[0].o), *b = vm_string_to_utf8(args[1].o); fprintf(stderr, "[log] %s: %s\n", a, b); free(a); free(b); } RETI(0); }
/* ---- MediaPlayer -> audio engine ---- */
typedef struct { Asset *a; AudioHandle h; int loop; float vol; uint32_t seek_ms; uint32_t res; } MP;
static struct { char *path; Asset *a; } acache[48]; static int nacache;
static char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET); char *b = malloc(l + 1);
    if (fread(b, 1, l, f) != (size_t)l) { free(b); fclose(f); return NULL; }
    fclose(f); *n = l; return b;
}
static Asset *load_asset(uint32_t resid) {
    const char *path = arsc_get(resid); if (!path) return NULL;
    for (int i = 0; i < nacache; i++) if (!strcmp(acache[i].path, path)) return acache[i].a;
    Asset *a = NULL; size_t n; const char *dot = strrchr(path, '.'), *sl = strrchr(path, '/'); const char *base = sl ? sl + 1 : path;
    if (dot && !strcmp(dot, ".mid")) { uint8_t *b = zip_extract(path, &n); if (b) { a = audio_load_midi(b, n); free(b); } }
    else {
        char p[600]; int bl = (int)(dot ? dot - base : strlen(base));
        /* bundled copy inside the VPK first, then the writable data folder */
        snprintf(p, sizeof p, "%saudio/%.*s.wav", plat_app_dir(), bl, base);
        char *b = slurp(p, &n); if (!b) { snprintf(p, sizeof p, "%saudio/%.*s.wav", plat_data_dir(), bl, base); b = slurp(p, &n); }
        if (b) { a = audio_load_wav((uint8_t *)b, n); free(b); }
        if (!a) fprintf(stderr, "[audio] no sound for %s (looked for audio/%.*s.wav)\n", path, bl, base);
    }
    if (nacache < 48) { acache[nacache].path = strdup(path); acache[nacache].a = a; nacache++; }
    return a;
}
NATIVE(n_mp_create) {
    Obj *o = vm_new_obj(C("Landroid/media/MediaPlayer;")); MP *m = calloc(1, sizeof *m);
    m->res = args[1].u; m->a = load_asset(args[1].u); audio_handle_init(&m->h); m->vol = 1.0f; o->data = m; RETO(o);
}
static MP *mp_of(Obj *o) { return (MP *)o->data; }
NATIVE(n_mp_start) { (void)ret; MP *m = mp_of(args[0].o); if (m && getenv("AVM_AUDIODBG")) fprintf(stderr, "[audio] start %s loop=%d%s\n", arsc_get(m->res), m->loop, m->a ? "" : " (NO DATA)"); if (m && m->a) audio_play(&m->h, m->a, m->loop, m->vol, m->seek_ms); }
NATIVE(n_mp_stop) { (void)ret; MP *m = mp_of(args[0].o); if (m) audio_stop(&m->h); }
NATIVE(n_mp_isPlaying) { MP *m = mp_of(args[0].o); RETI(m && audio_is_playing(&m->h)); }
NATIVE(n_mp_seekTo) { (void)ret; MP *m = mp_of(args[0].o); if (m) m->seek_ms = args[1].i < 0 ? 0 : (uint32_t)args[1].i; }
NATIVE(n_mp_setLooping) { (void)ret; MP *m = mp_of(args[0].o); if (m) m->loop = args[1].i != 0; }
NATIVE(n_mp_setVolume) { (void)ret; MP *m = mp_of(args[0].o); if (!m) return; float v = args[1].f; if (!(v >= 0)) v = 0; if (v > 1) v = 1; m->vol = v; audio_set_volume(&m->h, v); }
NATIVE(n_uri_parse) { (void)args; RETO(vm_new_obj(C("Landroid/net/Uri;"))); }
NATIVE(n_intent_chooser) { RETO(args[0].o); }
NATIVE(n_cb_shared) { (void)args; static Obj *s; RETO(singleton("Lcom/chartboost/sdk/Chartboost;", &s)); }
NATIVE(n_ret_false) { (void)args; RETI(0); }
NATIVE(n_builder_create) { (void)args; RETO(vm_new_obj(C("Landroid/app/AlertDialog;"))); }
NATIVE(n_me_getAction) { RETI(args[0].o->f[0].i); }
NATIVE(n_me_getIndex) { RETI(args[0].o->f[1].i); }
NATIVE(n_me_getCount) { RETI(args[0].o->f[2].i); }
NATIVE(n_me_getX0) { RETF(args[0].o->f[3].f); }
NATIVE(n_me_getY0) { RETF(args[0].o->f[4].f); }
NATIVE(n_me_getXi) { int i = args[1].i; if (i < 0 || i >= args[0].o->f[2].i || i > 5) i = 0; RETF(args[0].o->f[3 + 2 * i].f); }
NATIVE(n_me_getYi) { int i = args[1].i; if (i < 0 || i >= args[0].o->f[2].i || i > 5) i = 0; RETF(args[0].o->f[4 + 2 * i].f); }
NATIVE(n_ke_action) { RETI(args[0].o->f[0].i); }
NATIVE(n_ke_code) { RETI(args[0].o->f[1].i); }

/* events from the frontend ------------------------------------------------ */
static int in_hook;
void droid_touch(int action, int idx, int count, const float *xs, const float *ys) {
    if (!g_view || !g_view->f[2].o) return;
    Obj *l = g_view->f[2].o; Method *m = vm_find_method(l->cls, "onTouch", "(Landroid/view/View;Landroid/view/MotionEvent;)Z"); if (!m) return;
    Obj *e = vm_new_obj(C("Landroid/view/MotionEvent;"));
    SETI_(e->f[0], action); SETI_(e->f[1], idx); SETI_(e->f[2], count);
    for (int i = 0; i < count && i < 6; i++) { e->f[3 + 2 * i].j = 0; e->f[3 + 2 * i].f = xs[i]; e->f[4 + 2 * i].j = 0; e->f[4 + 2 * i].f = ys[i]; }
    Val a[3]; SETO_(a[0], l); SETO_(a[1], g_view); SETO_(a[2], e); Val r; vm_call(m, a, &r);
    if (vm_has_exception()) { vm_report_exception("onTouch"); vm_clear_exception(); }
}
void droid_key(int action, int keycode) {
    if (!g_view || !g_view->f[1].o) return;
    Obj *l = g_view->f[1].o; Method *m = vm_find_method(l->cls, "onKey", "(Landroid/view/View;ILandroid/view/KeyEvent;)Z"); if (!m) return;
    Obj *e = vm_new_obj(C("Landroid/view/KeyEvent;")); SETI_(e->f[0], action); SETI_(e->f[1], keycode);
    Val a[4]; SETO_(a[0], l); SETO_(a[1], g_view); SETI_(a[2], keycode); SETO_(a[3], e); Val r; vm_call(m, a, &r);
    if (vm_has_exception()) { vm_report_exception("onKey"); vm_clear_exception(); }
}
void app_frame_hook(int poll) {
    if (in_hook) return; in_hook = 1;
    if (poll) plat_poll();
    while (nhq) {
        Obj *r = hq[0]; memmove(hq, hq + 1, (--nhq) * sizeof(Obj *)); hq[nhq] = NULL;
        Method *m = vm_find_method(r->cls, "run", "()V");
        if (m) { Val a[1]; SETO_(a[0], r); vm_call(m, a, NULL); if (vm_has_exception()) { vm_report_exception("runnable"); vm_clear_exception(); } }
    }
    in_hook = 0;
}
void app_thread_start(Obj *t) { g_thread = t; vm_add_root(&g_thread); }

/* ------------------------------------------------------------ registration -- */
#define SIG(s, f) {s, f}
static const NativeClassDef dclasses[] = {
    {"Landroid/graphics/Bitmap;", NULL, "w h cfg mut"}, {"Landroid/graphics/Canvas;", NULL, "bmp"},
    {"Landroid/graphics/Rect;", NULL, "left top right bottom"}, {"Landroid/graphics/RectF;", NULL, "left top right bottom"},
    {"Landroid/view/SurfaceView;", NULL, "holder key touch"}, {"Landroid/view/SurfaceHolder;", NULL, "cb"},
    {"Landroid/view/MotionEvent;", NULL, "action aidx count x0 y0 x1 y1 x2 y2 x3 y3 x4 y4 x5 y5"},
    {"Landroid/view/KeyEvent;", NULL, "action code"}, {"Landroid/content/pm/PackageInfo;", NULL, "versionName versionCode"},
    {NULL, NULL, NULL}};
static const NativeDef dnatives[] = {
    SIG("Landroid/app/Activity;.<init>()V", n_nop), SIG("Landroid/app/Activity;.getResources()Landroid/content/res/Resources;", n_act_getRes),
    SIG("Landroid/app/Activity;.onBackPressed()V", n_act_finish), SIG("Landroid/app/Activity;.onCreate(Landroid/os/Bundle;)V", n_nop),
    SIG("Landroid/app/Activity;.onDestroy()V", n_nop), SIG("Landroid/app/Activity;.onPause()V", n_nop), SIG("Landroid/app/Activity;.onResume()V", n_nop),
    SIG("Landroid/app/Activity;.onPrepareDialog(ILandroid/app/Dialog;)V", n_nop), SIG("Landroid/app/Activity;.onRestart()V", n_nop),
    SIG("Landroid/app/Activity;.onStart()V", n_nop), SIG("Landroid/app/Activity;.onStop()V", n_nop),
    SIG("Landroid/app/Activity;.startActivity(Landroid/content/Intent;)V", n_nop), SIG("Landroid/app/Activity;.finish()V", n_act_finish),
    SIG("Landroid/app/Activity;.getAssets()Landroid/content/res/AssetManager;", n_act_getAssets),
    SIG("Landroid/app/Activity;.getPackageManager()Landroid/content/pm/PackageManager;", n_act_getPM),
    SIG("Landroid/app/Activity;.getPackageName()Ljava/lang/String;", n_act_pkg),
    SIG("Landroid/app/Activity;.getPreferences(I)Landroid/content/SharedPreferences;", n_act_getPrefs),
    SIG("Landroid/app/Activity;.getSystemService(Ljava/lang/String;)Ljava/lang/Object;", n_act_getSys),
    SIG("Landroid/app/Activity;.getWindow()Landroid/view/Window;", n_act_getWindow), SIG("Landroid/app/Activity;.getWindowManager()Landroid/view/WindowManager;", n_act_getWM),
    SIG("Landroid/app/Activity;.requestWindowFeature(I)Z", n_act_reqFeature), SIG("Landroid/app/Activity;.runOnUiThread(Ljava/lang/Runnable;)V", n_runOnUi),
    SIG("Landroid/app/Activity;.setContentView(Landroid/view/View;)V", n_act_setContent), SIG("Landroid/app/Activity;.setRequestedOrientation(I)V", n_nop),
    SIG("Landroid/app/Activity;.showDialog(I)V", n_nop),
    SIG("Landroid/app/AlertDialog$Builder;.<init>(Landroid/content/Context;)V", n_nop), SIG("Landroid/app/AlertDialog$Builder;.create()Landroid/app/AlertDialog;", n_builder_create),
    SIG("Landroid/app/AlertDialog$Builder;.setMessage(Ljava/lang/CharSequence;)Landroid/app/AlertDialog$Builder;", n_ret_this),
    SIG("Landroid/app/AlertDialog$Builder;.setNegativeButton(Ljava/lang/CharSequence;Landroid/content/DialogInterface$OnClickListener;)Landroid/app/AlertDialog$Builder;", n_ret_this),
    SIG("Landroid/app/AlertDialog$Builder;.setPositiveButton(Ljava/lang/CharSequence;Landroid/content/DialogInterface$OnClickListener;)Landroid/app/AlertDialog$Builder;", n_ret_this),
    SIG("Landroid/app/AlertDialog$Builder;.setTitle(Ljava/lang/CharSequence;)Landroid/app/AlertDialog$Builder;", n_ret_this),
    SIG("Landroid/app/AlertDialog;.setMessage(Ljava/lang/CharSequence;)V", n_nop),
    SIG("Landroid/content/Intent;.<init>(Ljava/lang/String;)V", n_nop), SIG("Landroid/content/Intent;.createChooser(Landroid/content/Intent;Ljava/lang/CharSequence;)Landroid/content/Intent;", n_intent_chooser),
    SIG("Landroid/content/Intent;.putExtra(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;", n_ret_this),
    SIG("Landroid/content/Intent;.setData(Landroid/net/Uri;)Landroid/content/Intent;", n_ret_this), SIG("Landroid/content/Intent;.setType(Ljava/lang/String;)Landroid/content/Intent;", n_ret_this),
    SIG("Landroid/content/SharedPreferences;.getString(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", n_prefs_getString),
    SIG("Landroid/content/pm/PackageManager;.getPackageInfo(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;", n_pm_getInfo),
    SIG("Landroid/content/res/AssetManager;.open(Ljava/lang/String;)Ljava/io/InputStream;", n_assets_open), SIG("Landroid/content/res/Resources;.getString(I)Ljava/lang/String;", n_res_getString),
    SIG("Landroid/graphics/Bitmap$Config;.equals(Ljava/lang/Object;)Z", n_cfg_equals),
    SIG("Landroid/graphics/Bitmap;.copy(Landroid/graphics/Bitmap$Config;Z)Landroid/graphics/Bitmap;", n_bm_copy),
    SIG("Landroid/graphics/Bitmap;.createBitmap(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;", n_bm_createWHC),
    SIG("Landroid/graphics/Bitmap;.createBitmap(Landroid/graphics/Bitmap;IIII)Landroid/graphics/Bitmap;", n_bm_createSub),
    SIG("Landroid/graphics/Bitmap;.createBitmap(Landroid/graphics/Bitmap;IIIILandroid/graphics/Matrix;Z)Landroid/graphics/Bitmap;", n_bm_createMat),
    SIG("Landroid/graphics/Bitmap;.createBitmap([IIILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;", n_bm_createPix),
    SIG("Landroid/graphics/Bitmap;.getConfig()Landroid/graphics/Bitmap$Config;", n_bm_getConfig), SIG("Landroid/graphics/Bitmap;.getHeight()I", n_bm_getH),
    SIG("Landroid/graphics/Bitmap;.getPixels([IIIIIII)V", n_bm_getPixels), SIG("Landroid/graphics/Bitmap;.getWidth()I", n_bm_getW), SIG("Landroid/graphics/Bitmap;.isMutable()Z", n_bm_mutable),
    SIG("Landroid/graphics/BitmapFactory;.decodeByteArray([BII)Landroid/graphics/Bitmap;", n_bmf_decode),
    SIG("Landroid/graphics/Canvas;.<init>(Landroid/graphics/Bitmap;)V", n_cv_init),
    SIG("Landroid/graphics/Canvas;.clipRect(FFFFLandroid/graphics/Region$Op;)Z", n_cv_clipRectF), SIG("Landroid/graphics/Canvas;.clipRect(IIII)Z", n_cv_clipRectI),
    SIG("Landroid/graphics/Canvas;.drawArc(Landroid/graphics/RectF;FFZLandroid/graphics/Paint;)V", n_cv_drawArc),
    SIG("Landroid/graphics/Canvas;.drawBitmap(Landroid/graphics/Bitmap;FFLandroid/graphics/Paint;)V", n_cv_drawBitmap),
    SIG("Landroid/graphics/Canvas;.drawBitmap(Landroid/graphics/Bitmap;Landroid/graphics/Matrix;Landroid/graphics/Paint;)V", n_cv_drawBitmapM),
    SIG("Landroid/graphics/Canvas;.drawBitmap([IIIIIIIZLandroid/graphics/Paint;)V", n_cv_drawBitmapI),
    SIG("Landroid/graphics/Canvas;.drawLine(FFFFLandroid/graphics/Paint;)V", n_cv_drawLine), SIG("Landroid/graphics/Canvas;.drawPath(Landroid/graphics/Path;Landroid/graphics/Paint;)V", n_cv_drawPath),
    SIG("Landroid/graphics/Canvas;.drawRect(FFFFLandroid/graphics/Paint;)V", n_cv_drawRect), SIG("Landroid/graphics/Canvas;.drawRoundRect(Landroid/graphics/RectF;FFLandroid/graphics/Paint;)V", n_cv_drawRoundRect),
    SIG("Landroid/graphics/Canvas;.drawText(Ljava/lang/String;FFLandroid/graphics/Paint;)V", n_nop), SIG("Landroid/graphics/Canvas;.drawText(Ljava/lang/String;IIFFLandroid/graphics/Paint;)V", n_nop),
    SIG("Landroid/graphics/Canvas;.drawText([CIIFFLandroid/graphics/Paint;)V", n_nop),
    SIG("Landroid/graphics/Canvas;.getClipBounds()Landroid/graphics/Rect;", n_cv_getClipBounds), SIG("Landroid/graphics/Canvas;.restore()V", n_cv_restore),
    SIG("Landroid/graphics/Canvas;.save()I", n_cv_save), SIG("Landroid/graphics/Canvas;.scale(FF)V", n_cv_scale),
    SIG("Landroid/graphics/Color;.rgb(III)I", n_color_rgb), SIG("Landroid/graphics/DashPathEffect;.<init>([FF)V", n_nop),
    SIG("Landroid/graphics/Matrix;.<init>()V", n_mx_init), SIG("Landroid/graphics/Matrix;.postRotate(F)Z", n_mx_postRotate),
    SIG("Landroid/graphics/Matrix;.postTranslate(FF)Z", n_mx_postTranslate), SIG("Landroid/graphics/Matrix;.preTranslate(FF)Z", n_mx_preTranslate),
    SIG("Landroid/graphics/Matrix;.setScale(FF)V", n_mx_setScale),
    SIG("Landroid/graphics/Paint;.<init>()V", n_pt_init), SIG("Landroid/graphics/Paint;.getColor()I", n_pt_getColor),
    SIG("Landroid/graphics/Paint;.getTextBounds(Ljava/lang/String;IILandroid/graphics/Rect;)V", n_pt_getTextBounds),
    SIG("Landroid/graphics/Paint;.getTextBounds([CIILandroid/graphics/Rect;)V", n_pt_getTextBoundsC),
    SIG("Landroid/graphics/Paint;.setAntiAlias(Z)V", n_nop), SIG("Landroid/graphics/Paint;.setColor(I)V", n_pt_setColor),
    SIG("Landroid/graphics/Paint;.setPathEffect(Landroid/graphics/PathEffect;)Landroid/graphics/PathEffect;", n_ret_arg1),
    SIG("Landroid/graphics/Paint;.setStrokeWidth(F)V", n_pt_setStroke), SIG("Landroid/graphics/Paint;.setStyle(Landroid/graphics/Paint$Style;)V", n_pt_setStyle),
    SIG("Landroid/graphics/Paint;.setTextAlign(Landroid/graphics/Paint$Align;)V", n_pt_setAlign), SIG("Landroid/graphics/Paint;.setTextSize(F)V", n_pt_setTextSize),
    SIG("Landroid/graphics/Paint;.setTypeface(Landroid/graphics/Typeface;)Landroid/graphics/Typeface;", n_ret_arg1),
    SIG("Landroid/graphics/Path;.<init>()V", n_path_init), SIG("Landroid/graphics/Path;.lineTo(FF)V", n_path_lineTo), SIG("Landroid/graphics/Path;.moveTo(FF)V", n_path_moveTo),
    SIG("Landroid/graphics/Rect;.<init>()V", n_nop), SIG("Landroid/graphics/Rect;.height()I", n_rect_height), SIG("Landroid/graphics/Rect;.width()I", n_rect_width),
    SIG("Landroid/graphics/RectF;.<init>(FFFF)V", n_rectf_init4), SIG("Landroid/graphics/Typeface;.defaultFromStyle(I)Landroid/graphics/Typeface;", n_tf_default),
    SIG("Landroid/graphics/drawable/Drawable;.draw(Landroid/graphics/Canvas;)V", n_nop), SIG("Landroid/graphics/drawable/Drawable;.getIntrinsicHeight()I", n_ret0),
    SIG("Landroid/graphics/drawable/Drawable;.getIntrinsicWidth()I", n_ret0), SIG("Landroid/graphics/drawable/Drawable;.setBounds(IIII)V", n_nop),
    SIG("Landroid/media/MediaPlayer;.create(Landroid/content/Context;I)Landroid/media/MediaPlayer;", n_mp_create), SIG("Landroid/media/MediaPlayer;.isPlaying()Z", n_mp_isPlaying),
    SIG("Landroid/media/MediaPlayer;.seekTo(I)V", n_mp_seekTo), SIG("Landroid/media/MediaPlayer;.setLooping(Z)V", n_mp_setLooping), SIG("Landroid/media/MediaPlayer;.setVolume(FF)V", n_mp_setVolume),
    SIG("Landroid/media/MediaPlayer;.start()V", n_mp_start), SIG("Landroid/media/MediaPlayer;.stop()V", n_mp_stop),
    SIG("Landroid/net/Uri;.parse(Ljava/lang/String;)Landroid/net/Uri;", n_uri_parse),
    SIG("Landroid/os/Handler;.<init>()V", n_nop), SIG("Landroid/os/Handler;.post(Ljava/lang/Runnable;)Z", n_post), SIG("Landroid/os/Vibrator;.vibrate(J)V", n_nop),
    SIG("Landroid/util/Log;.d(Ljava/lang/String;Ljava/lang/String;)I", n_log_d),
    SIG("Landroid/view/Display;.getHeight()I", n_disp_h), SIG("Landroid/view/Display;.getWidth()I", n_disp_w),
    SIG("Landroid/view/KeyEvent;.getAction()I", n_ke_action), SIG("Landroid/view/KeyEvent;.getKeyCode()I", n_ke_code),
    SIG("Landroid/view/MotionEvent;.getActionIndex()I", n_me_getIndex), SIG("Landroid/view/MotionEvent;.getActionMasked()I", n_me_getAction),
    SIG("Landroid/view/MotionEvent;.getPointerCount()I", n_me_getCount), SIG("Landroid/view/MotionEvent;.getX()F", n_me_getX0), SIG("Landroid/view/MotionEvent;.getX(I)F", n_me_getXi),
    SIG("Landroid/view/MotionEvent;.getY()F", n_me_getY0), SIG("Landroid/view/MotionEvent;.getY(I)F", n_me_getYi),
    SIG("Landroid/view/SurfaceHolder;.addCallback(Landroid/view/SurfaceHolder$Callback;)V", n_sh_addCallback),
    SIG("Landroid/view/SurfaceHolder;.lockCanvas()Landroid/graphics/Canvas;", n_sh_lock), SIG("Landroid/view/SurfaceHolder;.unlockCanvasAndPost(Landroid/graphics/Canvas;)V", n_sh_unlock),
    SIG("Landroid/view/SurfaceView;.<init>(Landroid/content/Context;)V", n_sv_init), SIG("Landroid/view/SurfaceView;.getHolder()Landroid/view/SurfaceHolder;", n_sv_getHolder),
    SIG("Landroid/view/SurfaceView;.setFocusable(Z)V", n_nop), SIG("Landroid/view/SurfaceView;.setFocusableInTouchMode(Z)V", n_nop),
    SIG("Landroid/view/SurfaceView;.setOnKeyListener(Landroid/view/View$OnKeyListener;)V", n_sv_setKey), SIG("Landroid/view/SurfaceView;.setOnTouchListener(Landroid/view/View$OnTouchListener;)V", n_sv_setTouch),
    SIG("Landroid/view/Window;.addFlags(I)V", n_nop), SIG("Landroid/view/Window;.setFlags(II)V", n_nop),
    SIG("Landroid/view/WindowManager;.getDefaultDisplay()Landroid/view/Display;", n_wm_getDisplay),
    /* third-party SDKs: ads and analytics are simply switched off */
    SIG("Lcom/chartboost/sdk/Chartboost;.sharedChartboost()Lcom/chartboost/sdk/Chartboost;", n_cb_shared),
    SIG("Lcom/chartboost/sdk/Chartboost;.cacheInterstitial(Ljava/lang/String;)V", n_nop), SIG("Lcom/chartboost/sdk/Chartboost;.onBackPressed()Z", n_ret_false),
    SIG("Lcom/chartboost/sdk/Chartboost;.onCreate(Landroid/app/Activity;Ljava/lang/String;Ljava/lang/String;Lcom/chartboost/sdk/ChartboostDelegate;)V", n_nop),
    SIG("Lcom/chartboost/sdk/Chartboost;.onDestroy(Landroid/app/Activity;)V", n_nop), SIG("Lcom/chartboost/sdk/Chartboost;.onStart(Landroid/app/Activity;)V", n_nop),
    SIG("Lcom/chartboost/sdk/Chartboost;.onStop(Landroid/app/Activity;)V", n_nop), SIG("Lcom/chartboost/sdk/Chartboost;.showInterstitial()V", n_nop),
    SIG("Lcom/chartboost/sdk/Chartboost;.showMoreApps()V", n_nop), SIG("Lcom/chartboost/sdk/Chartboost;.startSession()V", n_nop),
    SIG("Lcom/flurry/android/FlurryAgent;.logEvent(Ljava/lang/String;)V", n_nop), SIG("Lcom/flurry/android/FlurryAgent;.logEvent(Ljava/lang/String;Ljava/util/Map;)V", n_nop),
    SIG("Lcom/flurry/android/FlurryAgent;.onEndSession(Landroid/content/Context;)V", n_nop), SIG("Lcom/flurry/android/FlurryAgent;.onStartSession(Landroid/content/Context;Ljava/lang/String;)V", n_nop),
    SIG("Lcom/google/ads/AdRequest;.<init>()V", n_nop), SIG("Lcom/google/ads/InterstitialAd;.<init>(Landroid/app/Activity;Ljava/lang/String;)V", n_nop),
    SIG("Lcom/google/ads/InterstitialAd;.loadAd(Lcom/google/ads/AdRequest;)V", n_nop), SIG("Lcom/google/ads/InterstitialAd;.setAdListener(Lcom/google/ads/AdListener;)V", n_nop),
    SIG("Lcom/google/ads/InterstitialAd;.show()V", n_nop), SIG("Lcom/google/ads/InterstitialAd;.stopLoading()V", n_nop),
    {NULL, NULL}};

#define ENUMMAKER(fn, cls) static Obj *fn(void) { return vm_new_obj(C(cls)); }
ENUMMAKER(mk_style_fill, "Landroid/graphics/Paint$Style;") ENUMMAKER(mk_style_stroke, "Landroid/graphics/Paint$Style;") ENUMMAKER(mk_style_fs, "Landroid/graphics/Paint$Style;")
ENUMMAKER(mk_align_l, "Landroid/graphics/Paint$Align;") ENUMMAKER(mk_align_c, "Landroid/graphics/Paint$Align;") ENUMMAKER(mk_align_r, "Landroid/graphics/Paint$Align;")
ENUMMAKER(mk_op_replace, "Landroid/graphics/Region$Op;") ENUMMAKER(mk_op_inter, "Landroid/graphics/Region$Op;") ENUMMAKER(mk_cfg_565, "Landroid/graphics/Bitmap$Config;")

void droid_register(void) {
    vm_register_native_classes(dclasses); vm_register_natives(dnatives);
    vm_register_statics("Landroid/graphics/Bitmap$Config;.ARGB_8888", mk_enum_bmcfg_a); vm_register_statics("Landroid/graphics/Bitmap$Config;.RGB_565", mk_cfg_565);
    vm_register_statics("Landroid/graphics/Paint$Style;.FILL", mk_style_fill); vm_register_statics("Landroid/graphics/Paint$Style;.STROKE", mk_style_stroke);
    vm_register_statics("Landroid/graphics/Paint$Style;.FILL_AND_STROKE", mk_style_fs);
    vm_register_statics("Landroid/graphics/Paint$Align;.LEFT", mk_align_l); vm_register_statics("Landroid/graphics/Paint$Align;.CENTER", mk_align_c); vm_register_statics("Landroid/graphics/Paint$Align;.RIGHT", mk_align_r);
    vm_register_statics("Landroid/graphics/Region$Op;.REPLACE", mk_op_replace); vm_register_statics("Landroid/graphics/Region$Op;.INTERSECT", mk_op_inter);
    (void)mk_enum_generic_obj; (void)n_ret_null;
}
void droid_reset(void) {
    g_screen_bmp = new_bitmap(SCR_W, SCR_H, 1); vm_add_root(&g_screen_bmp);
    g_screen_cv = vm_new_obj(C("Landroid/graphics/Canvas;")); vm_add_root(&g_screen_cv);
    Val a[2]; SETO_(a[0], g_screen_cv); SETO_(a[1], g_screen_bmp); n_cv_init(a, a);
    for (int i = 0; i < 64; i++) vm_add_root(&hq[i]);
    vm_add_root(&g_act);
}

/* ------------------------------------------------------------------- boot --- */
int droid_boot(const char *apk, const char *main_class) {
    if (zip_open_file(apk)) { fprintf(stderr, "cannot open apk %s\n", apk); return 1; }
    size_t n; uint8_t *dex = zip_extract("classes.dex", &n); if (!dex) { fprintf(stderr, "no classes.dex\n"); return 1; }
    size_t an; uint8_t *arsc = zip_extract("resources.arsc", &an); if (arsc) arsc_init(arsc, an);
    vm_init(); jlib_register(); droid_register();
    if (vm_load_dex(dex, n)) { fprintf(stderr, "bad dex\n"); return 1; }
    droid_reset();
    Class *mc = vm_class(main_class);
    Obj *act = vm_new_obj(mc); g_act = act;
    Val a[4], r; SETO_(a[0], act);
    Method *m = vm_find_method(mc, "<init>", "()V"); vm_call(m, a, &r);
    if (vm_has_exception()) { vm_report_exception("Main.<init>"); return 1; }
    SETO_(a[0], act); a[1].j = 0; m = vm_find_method(mc, "onCreate", "(Landroid/os/Bundle;)V"); vm_call(m, a, &r);
    if (vm_has_exception()) { vm_report_exception("Main.onCreate"); return 1; }
    SETO_(a[0], act); m = vm_find_method(mc, "onStart", "()V"); if (m) vm_call(m, a, &r);
    SETO_(a[0], act); m = vm_find_method(mc, "onResume", "()V"); if (m) vm_call(m, a, &r);
    if (vm_has_exception()) { vm_report_exception("Main.onResume"); return 1; }
    if (!g_view) { fprintf(stderr, "activity never set a content view\n"); return 1; }
    Obj *holder = g_view->f[0].o, *cb = holder ? holder->f[0].o : NULL;
    if (!cb) { fprintf(stderr, "no surface callback registered\n"); return 1; }
    SETO_(a[0], cb); SETO_(a[1], holder);
    m = vm_find_method(cb->cls, "surfaceCreated", "(Landroid/view/SurfaceHolder;)V"); if (m) vm_call(m, a, &r);
    SETO_(a[0], cb); SETO_(a[1], holder); SETI_(a[2], 4); SETI_(a[3], DISP_W);
    { Val b[5]; b[0] = a[0]; b[1] = a[1]; SETI_(b[2], 4); SETI_(b[3], DISP_W); SETI_(b[4], DISP_H);
      m = vm_find_method(cb->cls, "surfaceChanged", "(Landroid/view/SurfaceHolder;III)V"); if (m) vm_call(m, b, &r); }
    if (vm_has_exception()) { vm_report_exception("surface callbacks"); return 1; }
    if (!g_thread) { fprintf(stderr, "game thread never started\n"); return 1; }
    m = vm_find_method(g_thread->cls, "run", "()V"); SETO_(a[0], g_thread);
    vm_call(m, a, &r);
    if (vm_has_exception()) { vm_report_exception("GameThread.run"); return 1; }
    return 0;
}
