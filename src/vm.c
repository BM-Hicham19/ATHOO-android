/* avm: dex loader, class model, heap + GC, interpreter */
#include "vm.h"
#include <math.h>
#include <stdarg.h>
#include <stdint.h>

int vm_trace = 0, vm_exit_code = 0, vm_exiting = 0;

/* ================================================================== dex == */
static const uint8_t *D; static size_t DL;
static uint32_t n_str, n_type, n_proto, n_field, n_meth, n_class;
static const uint8_t *p_str, *p_type, *p_proto, *p_field, *p_meth, *p_class;

static inline uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint32_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t uleb(const uint8_t **pp) {
    const uint8_t *p = *pp; uint32_t r = 0; int s = 0;
    for (;;) { uint8_t b = *p++; r |= (uint32_t)(b & 0x7f) << s; s += 7; if (!(b & 0x80)) break; }
    *pp = p; return r;
}
static int32_t sleb(const uint8_t **pp) {
    const uint8_t *p = *pp; int32_t r = 0; int s = 0; uint8_t b;
    do { b = *p++; r |= (int32_t)(b & 0x7f) << s; s += 7; } while (b & 0x80);
    if (s < 32 && (b & 0x40)) r |= -(1 << s);
    *pp = p; return r;
}
static const char *dstr(uint32_t idx) {
    const uint8_t *p = D + rd32(p_str + 4 * idx);
    uleb(&p); return (const char *)p;
}
static const char *type_name(uint32_t t) { return dstr(rd32(p_type + 4 * t)); }

void vm_fatal(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); fprintf(stderr, "[avm] FATAL: "); vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n"); va_end(ap); fflush(stderr); exit(2);
}

/* ============================================================ registries == */
typedef struct NTab { const NativeDef *d; struct NTab *next; } NTab;
static NTab *ntabs;
void vm_register_natives(const NativeDef *defs) { NTab *t = malloc(sizeof *t); t->d = defs; t->next = ntabs; ntabs = t; }
typedef struct NCTab { const NativeClassDef *d; struct NCTab *next; } NCTab;
static NCTab *nctabs;
void vm_register_native_classes(const NativeClassDef *defs) { NCTab *t = malloc(sizeof *t); t->d = defs; t->next = nctabs; nctabs = t; }

static Obj ***roots; static int n_roots, cap_roots;
void vm_add_root(Obj **slot) {
    if (n_roots == cap_roots) { cap_roots = cap_roots ? cap_roots * 2 : 64; roots = realloc(roots, cap_roots * sizeof(Obj **)); }
    roots[n_roots++] = slot;
}
typedef struct SDef { const char *sig; Obj *(*mk)(void); Val v; int made; struct SDef *next; } SDef;
static SDef *sdefs;
void vm_register_statics(const char *sig, Obj *(*mk)(void)) {
    SDef *s = calloc(1, sizeof *s); s->sig = sig; s->mk = mk; s->next = sdefs; sdefs = s;
}
Val *vm_native_static(const char *sig) {
    for (SDef *s = sdefs; s; s = s->next)
        if (!strcmp(s->sig, sig)) {
            if (!s->made) { Obj *o = s->mk(); SETO_(s->v, o); s->made = 1; vm_add_root((Obj **)&s->v); }
            return &s->v;
        }
    return NULL;
}

/* ============================================================== classes == */
static Class **tcache; static int *def_of_type;
static Class *cls_object, *cls_string;
static Class **all_classes; static int n_all, cap_all;
Class *vm_cls_string(void) { return cls_string; }
static void note_class(Class *c) {
    if (n_all == cap_all) { cap_all = cap_all ? cap_all * 2 : 128; all_classes = realloc(all_classes, cap_all * sizeof(Class *)); }
    all_classes[n_all++] = c;
}
typedef struct NCache { Class *c; struct NCache *next; } NCache;
static NCache *ncache;
static Class *native_class(const char *name);

static const char *canon_class(const char *n) {
    static const char *al[][2] = {
        {"Ljava/util/List;", "Ljava/util/Vector;"}, {"Ljava/util/ArrayList;", "Ljava/util/Vector;"},
        {"Ljava/util/Collection;", "Ljava/util/Vector;"},
        {"Ljava/lang/StringBuffer;", "Ljava/lang/StringBuilder;"},
        {"Ljava/io/InputStream;", "Ljava/io/DataInputStream;"}, {"Ljava/io/ByteArrayInputStream;", "Ljava/io/DataInputStream;"},
        {"Ljava/io/FileInputStream;", "Ljava/io/DataInputStream;"},
        {"Ljava/io/OutputStream;", "Ljava/io/DataOutputStream;"}, {"Ljava/io/ByteArrayOutputStream;", "Ljava/io/DataOutputStream;"},
        {"Ljava/io/FileOutputStream;", "Ljava/io/DataOutputStream;"},
        {"Landroid/view/View;", "Landroid/view/SurfaceView;"},
        {"Landroid/content/Context;", "Landroid/app/Activity;"},
        {"Landroid/graphics/drawable/BitmapDrawable;", "Landroid/graphics/drawable/Drawable;"},
        {NULL, NULL}};
    for (int i = 0; al[i][0]; i++) if (!strcmp(n, al[i][0])) return al[i][1];
    return n;
}
static int is_throwable_name(const char *n) {
    size_t l = strlen(n);
    if (!strcmp(n, "Ljava/lang/Throwable;")) return 1;
    return (l > 10 && !strcmp(n + l - 10, "Exception;")) || (l > 6 && !strcmp(n + l - 6, "Error;"));
}
static const NativeClassDef *find_ncdef(const char *name) {
    for (NCTab *t = nctabs; t; t = t->next)
        for (const NativeClassDef *d = t->d; d->name; d++) if (!strcmp(d->name, name)) return d;
    return NULL;
}
static int count_words(const char *s) { int n = 0, in = 0; for (; s && *s; s++) { if (*s == ' ') in = 0; else if (!in) { in = 1; n++; } } return n; }

static Class *array_class(const char *name) {
    for (NCache *n = ncache; n; n = n->next) if (!strcmp(n->c->name, name)) return n->c;
    Class *c = calloc(1, sizeof *c); c->name = strdup(name); c->is_native = 1; c->def_idx = -1; c->init_state = 2;
    if (!cls_object) native_class("Ljava/lang/Object;");
    c->super = cls_object;
    switch (name[1]) {
        case 'B': case 'Z': c->elem_size = 1; break;
        case 'C': case 'S': c->elem_size = 2; break;
        case 'I': case 'F': c->elem_size = 4; break;
        case 'J': case 'D': c->elem_size = 8; break;
        default: c->elem_size = 0; break;
    }
    NCache *n = malloc(sizeof *n); n->c = c; n->next = ncache; ncache = n;
    return c;
}
static Class *native_class(const char *name) {
    if (name[0] == '[') return array_class(name);
    for (NCache *n = ncache; n; n = n->next) if (!strcmp(n->c->name, name)) return n->c;
    Class *c = calloc(1, sizeof *c);
    c->name = strdup(name); c->is_native = 1; c->def_idx = -1; c->init_state = 2;
    NCache *n = malloc(sizeof *n); n->c = c; n->next = ncache; ncache = n;
    if (!strcmp(name, "Ljava/lang/Object;")) { cls_object = c; return c; }
    if (!cls_object) native_class("Ljava/lang/Object;");
    const NativeClassDef *d = find_ncdef(name);
    const char *sup = d && d->super ? d->super : NULL;
    if (!sup) {
        if (is_throwable_name(name)) {
            if (!strcmp(name, "Ljava/lang/Throwable;")) sup = "Ljava/lang/Object;";
            else if (!strcmp(name, "Ljava/lang/Exception;") || !strcmp(name, "Ljava/lang/Error;")) sup = "Ljava/lang/Throwable;";
            else if (strstr(name, "Error;")) sup = "Ljava/lang/Error;";
            else sup = "Ljava/lang/Exception;";
        } else sup = "Ljava/lang/Object;";
    }
    c->super = native_class(sup);
    c->ninst = c->super->ninst + (d ? count_words(d->fields) : 0);
    if (!strcmp(name, "Ljava/lang/String;")) cls_string = c;
    return c;
}
int vm_native_field_slot(Class *c, const char *fname) {
    for (; c; c = c->super) {
        const NativeClassDef *d = find_ncdef(c->name);
        if (!d || !d->fields) continue;
        int base = c->super ? c->super->ninst : 0, i = 0;
        const char *p = d->fields;
        while (*p) {
            while (*p == ' ') p++;
            if (!*p) break;
            const char *q = p; while (*q && *q != ' ') q++;
            if ((size_t)(q - p) == strlen(fname) && !strncmp(p, fname, q - p)) return base + i;
            i++; p = q;
        }
    }
    return -1;
}

/* protos */
static char **proto_cache;
static const char *proto_desc(uint32_t pidx) {
    if (proto_cache[pidx]) return proto_cache[pidx];
    const uint8_t *pi = p_proto + 12 * pidx;
    char buf[512]; size_t n = 0; buf[n++] = '(';
    uint32_t po = rd32(pi + 8);
    if (po) {
        uint32_t cnt = rd32(D + po);
        for (uint32_t i = 0; i < cnt; i++) { const char *t = type_name(rd16(D + po + 4 + 2 * i)); size_t l = strlen(t); memcpy(buf + n, t, l); n += l; }
    }
    buf[n++] = ')';
    const char *r = type_name(rd32(pi + 4)); size_t l = strlen(r); memcpy(buf + n, r, l); n += l; buf[n] = 0;
    return proto_cache[pidx] = strdup(buf);
}

/* encoded values for static initialisers */
static Obj *get_string(uint32_t idx);
static void parse_encoded(const uint8_t **pp, Val *out) {
    const uint8_t *p = *pp; uint8_t h = *p++; int type = h & 0x1f, arg = h >> 5;
    int64_t sv = 0; uint64_t uv = 0;
    switch (type) {
        case 0x00: case 0x02: case 0x04: case 0x06: {
            for (int i = 0; i <= arg; i++) uv |= (uint64_t)*p++ << (8 * i);
            int bits = 8 * (arg + 1); sv = (int64_t)(uv << (64 - bits)) >> (64 - bits);
            out->j = (type == 0x06) ? sv : (int64_t)(int32_t)sv; break; }
        case 0x03: case 0x17: case 0x18: case 0x19: case 0x1a: case 0x1b: {
            for (int i = 0; i <= arg; i++) uv |= (uint64_t)*p++ << (8 * i);
            if (type == 0x17) SETO_(*out, get_string((uint32_t)uv)); else out->j = (int64_t)uv; break; }
        case 0x10: { uint32_t b = 0; for (int i = 0; i <= arg; i++) b |= (uint32_t)*p++ << (8 * i); b <<= 8 * (3 - arg); out->j = 0; memcpy(&out->f, &b, 4); break; }
        case 0x11: { uint64_t b = 0; for (int i = 0; i <= arg; i++) b |= (uint64_t)*p++ << (8 * i); b <<= 8 * (7 - arg); memcpy(&out->d, &b, 8); break; }
        case 0x1e: out->j = 0; break;
        case 0x1f: out->j = arg; break;
        default: vm_fatal("unsupported encoded value type 0x%x", type);
    }
    *pp = p;
}

static Class *class_for_type(uint32_t t);
static Method *add_methods(Class *c, const uint8_t **pp, int count, uint32_t *idx_base, Method *arr, int off) {
    const uint8_t *p = *pp; uint32_t midx = 0;
    for (int i = 0; i < count; i++) {
        midx += uleb(&p); uint32_t acc = uleb(&p), co = uleb(&p);
        Method *m = &arr[off + i]; const uint8_t *mi = p_meth + 8 * midx;
        m->cls = c; m->name_idx = rd32(mi + 4); m->proto_idx = rd16(mi + 2); m->name = dstr(m->name_idx);
        m->access = acc; m->code = co ? D + co : NULL;
        const uint8_t *pi = p_proto + 12 * m->proto_idx;
        m->shorty = dstr(rd32(pi));
        m->nins = (acc & 8) ? 0 : 1;
        for (const char *s = m->shorty + 1; *s; s++) m->nins += (*s == 'J' || *s == 'D') ? 2 : 1;
        if (co) m->nregs = rd16(D + co);
        { char b[300]; snprintf(b, sizeof b, "%s.%s%s", c->name, m->name, proto_desc(m->proto_idx)); m->sig = strdup(b); }
    }
    *pp = p; return arr;
}
static void parse_class_data(Class *c, const uint8_t *cd, uint32_t statics_off) {
    const uint8_t *p = cd;
    uint32_t nsf = uleb(&p), nif = uleb(&p), ndm = uleb(&p), nvm = uleb(&p);
    c->sfields = calloc(nsf + 1, sizeof(Field)); c->nsf = nsf; c->statics = calloc(nsf + 1, sizeof(Val));
    c->ifields = calloc(nif + 1, sizeof(Field)); c->nif = nif;
    uint32_t fidx = 0;
    for (uint32_t i = 0; i < nsf; i++) { fidx += uleb(&p); uleb(&p); const uint8_t *fi = p_field + 8 * fidx; c->sfields[i].type_idx = rd16(fi + 2); c->sfields[i].name_idx = rd32(fi + 4); c->sfields[i].slot = i; }
    fidx = 0;
    for (uint32_t i = 0; i < nif; i++) { fidx += uleb(&p); uleb(&p); const uint8_t *fi = p_field + 8 * fidx; c->ifields[i].type_idx = rd16(fi + 2); c->ifields[i].name_idx = rd32(fi + 4); c->ifields[i].slot = c->ninst + i; }
    c->ninst += nif;
    c->nmeth = ndm + nvm; c->methods = calloc(c->nmeth + 1, sizeof(Method));
    add_methods(c, &p, ndm, NULL, c->methods, 0);
    add_methods(c, &p, nvm, NULL, c->methods, ndm);
    if (statics_off) {
        const uint8_t *sp = D + statics_off; uint32_t n = uleb(&sp);
        for (uint32_t i = 0; i < n && i < nsf; i++) parse_encoded(&sp, &c->statics[i]);
    }
    note_class(c);
}
static Class *build_app_class(uint32_t tidx, int di) {
    const uint8_t *cdef = p_class + 32 * di;
    Class *c = calloc(1, sizeof *c);
    tcache[tidx] = c;
    c->name = type_name(tidx); c->def_idx = di;
    { size_t nl = strlen(c->name); for (NTab *t = ntabs; t && !c->override_native; t = t->next) for (const NativeDef *d = t->d; d->sig; d++) if (!strncmp(d->sig, c->name, nl) && d->sig[nl] == '.') { c->override_native = 1; break; } }
    uint32_t sup = rd32(cdef + 8);
    c->super = sup == 0xffffffffu ? NULL : class_for_type(sup);
    uint32_t ifo = rd32(cdef + 12);
    if (ifo) {
        uint32_t n = rd32(D + ifo); c->nifaces = n; c->ifaces = malloc(n * sizeof(Class *));
        for (uint32_t k = 0; k < n; k++) c->ifaces[k] = class_for_type(rd16(D + ifo + 4 + 2 * k));
    }
    c->ninst = c->super ? c->super->ninst : 0;
    uint32_t cdo = rd32(cdef + 24), sv = rd32(cdef + 28);
    if (cdo) parse_class_data(c, D + cdo, sv); else note_class(c);
    return c;
}
static Class *class_for_type(uint32_t t) {
    if (tcache[t]) return tcache[t];
    if (def_of_type[t] >= 0) return build_app_class(t, def_of_type[t]);
    return tcache[t] = native_class(type_name(t));
}
Class *vm_class(const char *name) {
    for (uint32_t i = 0; i < n_type; i++) if (!strcmp(type_name(i), name)) return class_for_type(i);
    return native_class(name);
}

/* =============================================================== heap/GC == */
static Obj *heap_head; static size_t heap_bytes, gc_next = 48u << 20; static long n_objs;
static Obj **g_interned; static Obj *g_exc, *g_caught;
static Val *stack, *sp, *stack_end;

static Obj *raw_obj(Class *c, int kind, int nf, size_t databytes) {
    Obj *o = calloc(1, sizeof(Obj));
    if (!o) vm_fatal("out of memory");
    o->cls = c; o->kind = kind; o->nf = nf;
    if (nf) { o->f = calloc(nf, sizeof(Val)); if (!o->f) vm_fatal("out of memory"); }
    if (databytes) { o->data = calloc(1, databytes); if (!o->data) vm_fatal("out of memory (%zu)", databytes); }
    o->next = heap_head; heap_head = o; n_objs++;
    heap_bytes += sizeof(Obj) + nf * sizeof(Val) + databytes;
    return o;
}
void vm_note_alloc(size_t n) { heap_bytes += n; }
Obj *vm_new_obj(Class *c) { return raw_obj(c, K_INST, c->ninst, 0); }
Obj *vm_new_array(Class *ac, int len) {
    size_t es = ac->elem_size ? (size_t)ac->elem_size : sizeof(Obj *);
    Obj *o = raw_obj(ac, K_ARRAY, 0, es * (len ? len : 1));
    o->len = len; o->esz = ac->elem_size ? ac->elem_size : E_REF;
    return o;
}
Obj *vm_new_array_t(const char *desc, int len) { return vm_new_array(array_class(desc), len); }
Obj *vm_new_string_u16(const uint16_t *c, int n) {
    if (!cls_string) native_class("Ljava/lang/String;");
    Obj *o = raw_obj(cls_string, K_STRING, 0, 2 * (n ? n : 1));
    o->len = n; if (n) memcpy(o->data, c, 2 * n);
    return o;
}
static Obj *new_string_mutf8(const char *s) {
    size_t l = strlen(s); uint16_t *buf = malloc(2 * (l + 1)); int n = 0;
    const uint8_t *p = (const uint8_t *)s;
    while (*p) {
        uint32_t c = *p++;
        if (c < 0x80) buf[n++] = c;
        else if ((c & 0xe0) == 0xc0) { c = (c & 0x1f) << 6 | (*p++ & 0x3f); buf[n++] = c; }
        else if ((c & 0xf0) == 0xe0) { c = (c & 0x0f) << 12 | (p[0] & 0x3f) << 6 | (p[1] & 0x3f); p += 2; buf[n++] = c; }
        else if ((c & 0xf8) == 0xf0) { c = (c & 7) << 18 | (p[0] & 0x3f) << 12 | (p[1] & 0x3f) << 6 | (p[2] & 0x3f); p += 3; c -= 0x10000; buf[n++] = 0xd800 | (c >> 10); buf[n++] = 0xdc00 | (c & 0x3ff); }
        else buf[n++] = '?';
    }
    Obj *o = vm_new_string_u16(buf, n); free(buf); return o;
}
Obj *vm_new_string_utf8(const char *s) { return new_string_mutf8(s); }
char *vm_string_to_utf8(Obj *s) {
    if (!s) return strdup("null");
    char *out = malloc(s->len * 3 + 1); int n = 0; uint16_t *c = STR_C(s);
    for (int i = 0; i < s->len; i++) {
        uint32_t u = c[i];
        if (u >= 0xd800 && u < 0xdc00 && i + 1 < s->len) { u = 0x10000 + ((u - 0xd800) << 10) + (c[i + 1] - 0xdc00); i++; }
        if (u < 0x80) out[n++] = u;
        else if (u < 0x800) { out[n++] = 0xc0 | u >> 6; out[n++] = 0x80 | (u & 0x3f); }
        else if (u < 0x10000) { out[n++] = 0xe0 | u >> 12; out[n++] = 0x80 | ((u >> 6) & 0x3f); out[n++] = 0x80 | (u & 0x3f); }
        else { out[n++] = 0xf0 | u >> 18; out[n++] = 0x80 | ((u >> 12) & 0x3f); out[n++] = 0x80 | ((u >> 6) & 0x3f); out[n++] = 0x80 | (u & 0x3f); }
    }
    out[n] = 0; return out;
}
static Obj *get_string(uint32_t idx) {
    if (!g_interned[idx]) g_interned[idx] = new_string_mutf8(dstr(idx));
    return g_interned[idx];
}
static Obj **intern_extra; static int n_intern_extra, cap_intern_extra;
Obj *vm_intern_utf8(const char *s) {
    Obj *o = new_string_mutf8(s);
    if (n_intern_extra == cap_intern_extra) { cap_intern_extra = cap_intern_extra ? cap_intern_extra * 2 : 64; intern_extra = realloc(intern_extra, cap_intern_extra * sizeof(Obj *)); }
    intern_extra[n_intern_extra++] = o; return o;
}

/* -- GC (stop the world, conservative for stacks / fields) -- */
static Obj **gc_sorted; static long gc_n; static Obj **mstack; static long mstack_n, mstack_cap;
static int cmp_ptr(const void *a, const void *b) { Obj *x = *(Obj *const *)a, *y = *(Obj *const *)b; return x < y ? -1 : x > y; }
static int is_heap(Obj *p) {
    if (!p || (uintptr_t)p & 3) return 0;
    long lo = 0, hi = gc_n - 1;
    while (lo <= hi) { long mid = (lo + hi) >> 1; if (gc_sorted[mid] == p) return 1; if (gc_sorted[mid] < p) lo = mid + 1; else hi = mid - 1; }
    return 0;
}
static void mpush(Obj *o) {
    if (mstack_n == mstack_cap) { mstack_cap = mstack_cap ? mstack_cap * 2 : 4096; mstack = realloc(mstack, mstack_cap * sizeof(Obj *)); }
    mstack[mstack_n++] = o;
}
static inline void mark_val(Val v) { Obj *p = v.o; if (p && is_heap(p) && !p->mark) { p->mark = 1; mpush(p); } }
static void mark_ptr(Obj *p) { if (p && !p->mark) { p->mark = 1; mpush(p); } }
void vm_gc(void) {
    gc_sorted = malloc((n_objs + 1) * sizeof(Obj *)); gc_n = 0;
    for (Obj *o = heap_head; o; o = o->next) { o->mark = 0; gc_sorted[gc_n++] = o; }
    qsort(gc_sorted, gc_n, sizeof(Obj *), cmp_ptr);
    for (Val *v = stack; v < sp + 16 && v < stack_end; v++) mark_val(*v);
    mark_ptr(g_exc); mark_ptr(g_caught);
    for (int i = 0; i < n_roots; i++) mark_val(*(Val *)roots[i]);
    for (uint32_t i = 0; i < n_str; i++) mark_ptr(g_interned[i]);
    for (int i = 0; i < n_intern_extra; i++) mark_ptr(intern_extra[i]);
    for (int i = 0; i < n_all; i++) for (int k = 0; k < all_classes[i]->nsf; k++) mark_val(all_classes[i]->statics[k]);
    while (mstack_n) {
        Obj *o = mstack[--mstack_n];
        if (o->kind == K_ARRAY && o->esz == E_REF) { Obj **e = ARR_R(o); for (int i = 0; i < o->len; i++) mark_ptr(e[i]); }
        for (int i = 0; i < o->nf; i++) mark_val(o->f[i]);
    }
    Obj **pp = &heap_head; size_t live = 0; long freed = 0;
    while (*pp) {
        Obj *o = *pp;
        if (o->mark) { live += sizeof(Obj) + o->nf * sizeof(Val); if (o->data) live += o->kind == K_ARRAY ? (size_t)o->len * (o->esz == E_REF ? sizeof(Obj *) : o->esz) : o->kind == K_STRING ? (size_t)o->len * 2 : (o->cap > 0 ? (size_t)o->cap : 256); pp = &o->next; }
        else { *pp = o->next; free(o->f); free(o->data); free(o); n_objs--; freed++; }
    }
    free(gc_sorted); gc_sorted = NULL; gc_n = 0;
    heap_bytes = live; { size_t base = getenv("AVM_GC_KB") ? ((size_t)atol(getenv("AVM_GC_KB")) << 10) : (48u << 20); gc_next = live + base; }
    if (vm_trace) fprintf(stderr, "[gc] freed %ld, live %ld objs, ~%zu KB\n", freed, n_objs, live >> 10);
}

/* ============================================================== exceptions == */
static const Method *cstack[256]; static int cdepth;
static char exc_where[512];
static void set_exc(Obj *e) {
    g_exc = e; exc_where[0] = 0; size_t n = 0;
    for (int i = cdepth - 1; i >= 0 && i > cdepth - 7 && n < sizeof exc_where - 100; i--)
        n += snprintf(exc_where + n, sizeof exc_where - n, "\n    at %s", cstack[i < 256 ? i : 255]->sig);
}
void vm_throw(const char *cd, const char *msg) {
    Class *c = native_class(cd);
    Obj *e = vm_new_obj(c);
    if (msg) e->data = strdup(msg);
    set_exc(e);
    if (getenv("AVM_THROWLOG")) fprintf(stderr, "[throw] %s: %s%s\n", cd, msg ? msg : "", exc_where);
}
int vm_has_exception(void) { return g_exc != NULL; }
void vm_clear_exception(void) { g_exc = NULL; }
void vm_report_exception(const char *where) {
    if (!g_exc) return;
    fprintf(stderr, "[avm] uncaught %s: %s (%s)%s\n", g_exc->cls->name, (g_exc->data && g_exc->cls->is_native) ? (char *)g_exc->data : "", where, exc_where);
}

int vm_is_instance(Obj *o, Class *t);
static int class_assignable(Class *from, Class *to) {
    if (from == to || to == cls_object) return 1;
    if (from->name[0] == '[' || to->name[0] == '[') return from->name[0] == '[' && to->name[0] == '[' && (!strcmp(from->name, to->name) || (from->name[1] != '[' ? from->name[1] == 'L' && to->name[1] == 'L' : to->name[1] == '[' || to->name[1] == 'L'));
    for (Class *k = from; k; k = k->super) {
        if (k == to) return 1;
        for (int i = 0; i < k->nifaces; i++) if (class_assignable(k->ifaces[i], to)) return 1;
        if (k->is_native && to->is_native && !strcmp(canon_class(k->name), canon_class(to->name))) return 1;
    }
    /* natives (framework types we don't model precisely) are accepted leniently */
    if (to->is_native && from->is_native && !is_throwable_name(to->name)) return 1;
    return 0;
}
int vm_is_instance(Obj *o, Class *t) { return o && class_assignable(o->cls, t); }

/* ======================================================== method resolution == */
static Method **mres;                     /* by method idx: declared resolution */
typedef struct VC { Class *c; uint32_t midx; Method *m; } VC;
static VC vcache[8192];

static NativeFn native_find(const char *cls, const char *name, const char *proto) {
    char buf[400]; snprintf(buf, sizeof buf, "%s.%s%s", canon_class(cls), name, proto);
    for (NTab *t = ntabs; t; t = t->next) for (const NativeDef *d = t->d; d->sig; d++) if (!strcmp(d->sig, buf)) return d->fn;
    /* throwable family */
    if (is_throwable_name(cls)) {
        snprintf(buf, sizeof buf, "Ljava/lang/Throwable;.%s%s", name, proto);
        for (NTab *t = ntabs; t; t = t->next) for (const NativeDef *d = t->d; d->sig; d++) if (!strcmp(d->sig, buf)) return d->fn;
    }
    return NULL;
}
static Method *find_in_class(Class *c, uint32_t nidx, uint32_t pidx) {
    for (int i = 0; i < c->nmeth; i++) if (c->methods[i].name_idx == nidx && c->methods[i].proto_idx == pidx) return &c->methods[i];
    return NULL;
}
static Method *make_native(Class *c, const char *name, const char *proto, uint32_t pidx, NativeFn fn) {
    Method *m = calloc(1, sizeof *m);
    m->cls = c; m->name = name; m->native = fn; m->proto_idx = pidx;
    char b[400]; snprintf(b, sizeof b, "%s.%s%s", c->name, name, proto); m->sig = strdup(b);
    return m;
}
/* search class chain starting at c (used for declared / super lookups) */
static Method *lookup_chain(Class *c, uint32_t midx, int allow_missing) {
    const uint8_t *mi = p_meth + 8 * midx;
    uint32_t pidx = rd16(mi + 2), nidx = rd32(mi + 4);
    const char *name = dstr(nidx), *proto = proto_desc(pidx);
    for (Class *k = c; k; k = k->super) {
        if (!k->is_native) {
            if (k->override_native) { NativeFn fn = native_find(k->name, name, proto); if (fn) return make_native(k, name, proto, pidx, fn); }
            Method *m = find_in_class(k, nidx, pidx);
            if (m && (m->code || m->native)) return m;
        } else {
            NativeFn fn = native_find(k->name, name, proto);
            if (fn) return make_native(k, name, proto, pidx, fn);
        }
    }
    /* interfaces: look for concrete or abstract decl on interface types */
    for (Class *k = c; k; k = k->super)
        for (int i = 0; i < k->nifaces; i++) { Method *m = lookup_chain(k->ifaces[i], midx, 1); if (m) return m; }
    if (allow_missing) return NULL;
    Method *m = calloc(1, sizeof *m);       /* unimplemented stub */
    m->cls = c; m->name = name; m->proto_idx = pidx;
    char b[400]; snprintf(b, sizeof b, "%s.%s%s", type_name(rd16(mi)), name, proto); m->sig = strdup(b);
    m->shorty = dstr(rd32(p_proto + 12 * pidx));
    return m;
}
static Method *resolve_decl(uint32_t midx) {
    if (mres[midx]) return mres[midx];
    Class *c = class_for_type(rd16(p_meth + 8 * midx));
    return mres[midx] = lookup_chain(c, midx, 0);
}
static Method *resolve_virtual(Obj *recv, uint32_t midx) {
    Class *rc = recv->cls;
    uint32_t h = (((uintptr_t)rc >> 4) * 31 + midx) & 8191;
    if (vcache[h].c == rc && vcache[h].midx == midx) return vcache[h].m;
    Method *m = lookup_chain(rc, midx, 1);
    if (!m) m = resolve_decl(midx);
    vcache[h].c = rc; vcache[h].midx = midx; vcache[h].m = m;
    return m;
}
Method *vm_find_method(Class *c, const char *name, const char *protodesc) {
    for (Class *k = c; k; k = k->super)
        for (int i = 0; i < k->nmeth; i++)
            if (!strcmp(k->methods[i].name, name) && !strcmp(proto_desc(k->methods[i].proto_idx), protodesc)) return &k->methods[i];
    return NULL;
}

/* ================================================================ fields == */
typedef struct FRef { uint8_t kind; int slot; Val *sptr; } FRef;       /* 1 inst, 2 static */
static FRef *fres;
static void ensure_init(Class *c);
static Field *find_field(Class *c, uint32_t nidx, uint32_t tidx, int is_static, Class **owner) {
    for (Class *k = c; k; k = k->super) {
        if (k->is_native) continue;
        Field *arr = is_static ? k->sfields : k->ifields; int n = is_static ? k->nsf : k->nif;
        for (int i = 0; i < n; i++) if (arr[i].name_idx == nidx && arr[i].type_idx == tidx) { *owner = k; return &arr[i]; }
        if (is_static) for (int j = 0; j < k->nifaces; j++) { Field *f = find_field(k->ifaces[j], nidx, tidx, 1, owner); if (f) return f; }
    }
    return NULL;
}
static FRef *resolve_field(uint32_t fidx, int is_static) {
    FRef *r = &fres[fidx];
    if (r->kind) return r;
    const uint8_t *fi = p_field + 8 * fidx;
    Class *c = class_for_type(rd16(fi)); uint32_t tidx = rd16(fi + 2), nidx = rd32(fi + 4);
    Class *owner = NULL; Field *f = find_field(c, nidx, tidx, is_static, &owner);
    if (f) {
        if (is_static) { ensure_init(owner); r->sptr = &owner->statics[f->slot]; r->kind = 2; }
        else { r->slot = f->slot; r->kind = 1; }
        return r;
    }
    /* native class field */
    const char *fname = dstr(nidx);
    if (is_static) {
        char b[300]; snprintf(b, sizeof b, "%s.%s", c->name, fname);
        Val *v = vm_native_static(b);
        if (!v) { fprintf(stderr, "[avm] unimplemented static field %s\n", b); v = calloc(1, sizeof(Val)); }
        r->sptr = v; r->kind = 2;
    } else {
        int s = vm_native_field_slot(c, fname);
        if (s < 0) { fprintf(stderr, "[avm] unimplemented field %s.%s\n", c->name, fname); s = 0; }
        r->slot = s; r->kind = 1;
    }
    return r;
}

/* ============================================================ interpreter == */
static void interp(Method *m, Val *regs, Val *ret);
static const char *M_DEBUG(uint32_t midx) { static char b[400]; const uint8_t *mi = p_meth + 8 * midx; snprintf(b, sizeof b, "%s.%s%s", type_name(rd16(mi)), dstr(rd32(mi + 4)), proto_desc(rd16(mi + 2))); return b; }

static void ensure_init(Class *c) {
    if (!c || c->init_state) return;
    c->init_state = 1;
    if (c->super) ensure_init(c->super);
    if (!c->is_native && c->def_idx >= 0 && !c->override_native) {
        for (int i = 0; i < c->nmeth; i++)
            if (!strcmp(c->methods[i].name, "<clinit>")) { Val r; Val *av = sp; interp(&c->methods[i], av, &r); break; }
    }
    c->init_state = 2;
}

static void call_method(Method *m, Val *av, int n, Val *ret) {
    if (m->native) { Val *save = sp; sp = av + n; m->native(av, ret); sp = save; return; }
    if (!m->code) {
        static const char *seen[64]; static int ns;
        for (int i = 0; i < ns; i++) if (seen[i] == m->sig) { ret->j = 0; return; }
        if (ns < 64) seen[ns++] = m->sig;
        fprintf(stderr, "[avm] unimplemented native: %s\n", m->sig);
        ret->j = 0; return;
    }
    interp(m, av, ret);
}
void vm_call(Method *m, Val *args, Val *ret) {
    Val *av = sp; int n = m->nins;
    for (int i = 0; i < n; i++) av[i] = args[i];
    Val dummy; if (!ret) ret = &dummy;
    call_method(m, av, n, ret);
    sp = av;
}

static int find_handler(const uint8_t *code, uint32_t addr, Obj *e) {
    uint32_t tries = rd16(code + 6); if (!tries) return -1;
    uint32_t isz = rd32(code + 12);
    const uint8_t *t = code + 16 + 2 * isz; if (isz & 1) t += 2;
    const uint8_t *hbase = t + 8 * tries;
    for (uint32_t i = 0; i < tries; i++) {
        uint32_t start = rd32(t + 8 * i), cnt = rd16(t + 8 * i + 4), hoff = rd16(t + 8 * i + 6);
        if (addr >= start && addr < start + cnt) {
            const uint8_t *h = hbase + hoff; int32_t sz = sleb(&h); int n = sz < 0 ? -sz : sz;
            for (int k = 0; k < n; k++) { uint32_t ty = uleb(&h), ad = uleb(&h); if (vm_is_instance(e, class_for_type(ty))) return ad; }
            if (sz <= 0) return uleb(&h);
            return -1;
        }
    }
    return -1;
}

static inline int32_t f2i(float f) { if (f != f) return 0; if (f >= 2147483648.0f) return INT32_MAX; if (f <= -2147483648.0f) return INT32_MIN; return (int32_t)f; }
static inline int64_t f2l(double f) { if (f != f) return 0; if (f >= 9223372036854775807.0) return INT64_MAX; if (f <= -9223372036854775808.0) return INT64_MIN; return (int64_t)f; }
static inline int32_t d2i(double f) { if (f != f) return 0; if (f >= 2147483647.0) return INT32_MAX; if (f <= -2147483648.0) return INT32_MIN; return (int32_t)f; }

static size_t esize_of(Obj *a) { return a->esz == E_REF ? sizeof(Obj *) : a->esz; }

static void interp(Method *m, Val *regs, Val *ret) {
    const uint8_t *code = m->code;
    int nregs = m->nregs, nins = m->nins;
    uint32_t isz = rd32(code + 12);
    const uint16_t *insns = (const uint16_t *)(code + 16);
    if (regs + nregs + 300 > stack_end) { vm_throw("Ljava/lang/StackOverflowError;", "avm stack"); return; }
    if (nregs - nins > 0) { memmove(regs + nregs - nins, regs, nins * sizeof(Val)); memset(regs, 0, (nregs - nins) * sizeof(Val)); }
    Val *saved_sp = sp; sp = regs + nregs;
    if (cdepth < 256) cstack[cdepth] = m; cdepth++;
    if (vm_trace > 1) fprintf(stderr, "%*s> %s\n", cdepth > 40 ? 40 : cdepth, "", m->sig);
    if (heap_bytes > gc_next) vm_gc();
    const uint16_t *pc = insns, *ipc; Val result; result.j = 0;
    uint16_t inst;

#define A_  ((inst >> 8) & 0xf)
#define B_  (inst >> 12)
#define AA_ (inst >> 8)
#define SETI(r, x) (regs[r].j = (int64_t)(int32_t)(x))
#define SETJ(r, x) (regs[r].j = (int64_t)(x))
#define SETF(r, x) do { float t_ = (x); regs[r].j = 0; regs[r].f = t_; } while (0)
#define SETD(r, x) (regs[r].d = (x))
#define SETO(r, p) (regs[r].j = (int64_t)(intptr_t)(p))
#define THROW(cls, msg) do { vm_throw(cls, msg); goto exc; } while (0)
#define NPE() THROW("Ljava/lang/NullPointerException;", m->sig)
#define S16(x) ((int16_t)(x))
#define S32(p) ((int32_t)((uint32_t)(p)[0] | (uint32_t)(p)[1] << 16))
#define CHECK() do { if (g_exc) goto exc; } while (0)

    for (;;) {
        ipc = pc; inst = *pc;
        switch (inst & 0xff) {
        case 0x00: /* nop (and payload pseudo-ops are never executed) */ pc++; break;
        case 0x01: case 0x07: regs[A_] = regs[B_]; pc++; break;
        case 0x02: case 0x08: regs[AA_] = regs[pc[1]]; pc += 2; break;
        case 0x03: case 0x09: regs[pc[1]] = regs[pc[2]]; pc += 3; break;
        case 0x04: regs[A_] = regs[B_]; pc++; break;
        case 0x05: regs[AA_] = regs[pc[1]]; pc += 2; break;
        case 0x06: regs[pc[1]] = regs[pc[2]]; pc += 3; break;
        case 0x0a: case 0x0b: case 0x0c: regs[AA_] = result; pc++; break;
        case 0x0d: SETO(AA_, g_caught); pc++; break;
        case 0x0e: goto out;
        case 0x0f: case 0x10: case 0x11: *ret = regs[AA_]; goto out;
        case 0x12: { int32_t v = (int32_t)((uint32_t)B_ << 28) >> 28; SETI(A_, v); pc++; break; }
        case 0x13: SETI(AA_, S16(pc[1])); pc += 2; break;
        case 0x14: SETI(AA_, S32(pc + 1)); pc += 3; break;
        case 0x15: SETI(AA_, (uint32_t)pc[1] << 16); pc += 2; break;
        case 0x16: SETJ(AA_, (int64_t)S16(pc[1])); pc += 2; break;
        case 0x17: SETJ(AA_, (int64_t)S32(pc + 1)); pc += 3; break;
        case 0x18: SETJ(AA_, (uint64_t)pc[1] | (uint64_t)pc[2] << 16 | (uint64_t)pc[3] << 32 | (uint64_t)pc[4] << 48); pc += 5; break;
        case 0x19: SETJ(AA_, (uint64_t)pc[1] << 48); pc += 2; break;
        case 0x1a: SETO(AA_, get_string(pc[1])); pc += 2; break;
        case 0x1b: SETO(AA_, get_string((uint32_t)pc[1] | (uint32_t)pc[2] << 16)); pc += 3; break;
        case 0x1c: SETO(AA_, NULL); pc += 2; break;        /* const-class: unsupported (null) */
        case 0x1d: case 0x1e: if (!regs[AA_].o) NPE(); pc++; break;
        case 0x1f: { Obj *o = regs[AA_].o; if (o && !vm_is_instance(o, class_for_type(pc[1]))) THROW("Ljava/lang/ClassCastException;", o->cls->name); pc += 2; break; }
        case 0x20: { Obj *o = regs[B_].o; SETI(A_, o && vm_is_instance(o, class_for_type(pc[1]))); pc += 2; break; }
        case 0x21: { Obj *o = regs[B_].o; if (!o) NPE(); SETI(A_, o->len); pc++; break; }
        case 0x22: { Class *c = class_for_type(pc[1]); ensure_init(c); SETO(AA_, vm_new_obj(c)); pc += 2; break; }
        case 0x23: { int n = regs[B_].i; if (n < 0) THROW("Ljava/lang/NegativeArraySizeException;", ""); SETO(A_, vm_new_array(class_for_type(pc[1]), n)); pc += 2; break; }
        case 0x24: case 0x25: {
            int n, base = 0; uint16_t r = pc[2]; int rng = (inst & 0xff) == 0x25;
            n = rng ? AA_ : B_; if (!rng) base = 0;
            Class *ac = class_for_type(pc[1]); Obj *a = vm_new_array(ac, n);
            for (int i = 0; i < n; i++) {
                Val v;
                if (rng) v = regs[r + i]; else { int rr = i == 4 ? A_ : (r >> (4 * i)) & 0xf; v = regs[rr]; }
                switch (a->esz) { case E_REF: ARR_R(a)[i] = v.o; break; case E_4: ARR_I(a)[i] = v.i; break; case E_8: ((int64_t *)a->data)[i] = v.j; break; case E_2: ARR_C(a)[i] = v.i; break; default: ARR_B(a)[i] = v.i; }
            }
            (void)base; result.j = 0; SETO_(result, a); pc += 3; break; }
        case 0x26: {
            Obj *a = regs[AA_].o; if (!a) NPE();
            const uint16_t *pl = pc + S32(pc + 1);
            int w = pl[1]; uint32_t sz = pl[2] | (uint32_t)pl[3] << 16;
            if ((int)sz > a->len) THROW("Ljava/lang/ArrayIndexOutOfBoundsException;", "fill-array-data");
            memcpy(a->data, pl + 4, (size_t)w * sz); pc += 3; break; }
        case 0x27: { Obj *e = regs[AA_].o; if (!e) NPE(); set_exc(e); goto exc; }
        case 0x28: pc += (int8_t)(inst >> 8); break;
        case 0x29: pc += S16(pc[1]); break;
        case 0x2a: pc += S32(pc + 1); break;
        case 0x2b: {
            const uint16_t *pl = pc + S32(pc + 1); int32_t v = regs[AA_].i, first = S32(pl + 2); uint32_t sz = pl[1];
            int64_t k = (int64_t)v - first;
            if (k >= 0 && k < (int64_t)sz) pc += S32(pl + 4 + 2 * k); else pc += 3;
            break; }
        case 0x2c: {
            const uint16_t *pl = pc + S32(pc + 1); int32_t v = regs[AA_].i; uint32_t sz = pl[1]; int found = 0;
            for (uint32_t i = 0; i < sz; i++) if (S32(pl + 2 + 2 * i) == v) { pc += S32(pl + 2 + 2 * sz + 2 * i); found = 1; break; }
            if (!found) pc += 3;
            break; }
        case 0x2d: case 0x2e: { float a = regs[pc[1] & 0xff].f, b = regs[pc[1] >> 8].f; SETI(AA_, (a != a || b != b) ? ((inst & 0xff) == 0x2d ? -1 : 1) : a > b ? 1 : a == b ? 0 : -1); pc += 2; break; }
        case 0x2f: case 0x30: { double a = regs[pc[1] & 0xff].d, b = regs[pc[1] >> 8].d; SETI(AA_, (a != a || b != b) ? ((inst & 0xff) == 0x2f ? -1 : 1) : a > b ? 1 : a == b ? 0 : -1); pc += 2; break; }
        case 0x31: { int64_t a = regs[pc[1] & 0xff].j, b = regs[pc[1] >> 8].j; SETI(AA_, a > b ? 1 : a == b ? 0 : -1); pc += 2; break; }
#define IFC(op, cond) case op: { int64_t a = regs[A_].j, b = regs[B_].j; (void)a; (void)b; if (cond) pc += S16(pc[1]); else pc += 2; break; }
        IFC(0x32, a == b) IFC(0x33, a != b)
        case 0x34: if (regs[A_].i < regs[B_].i) pc += S16(pc[1]); else pc += 2; break;
        case 0x35: if (regs[A_].i >= regs[B_].i) pc += S16(pc[1]); else pc += 2; break;
        case 0x36: if (regs[A_].i > regs[B_].i) pc += S16(pc[1]); else pc += 2; break;
        case 0x37: if (regs[A_].i <= regs[B_].i) pc += S16(pc[1]); else pc += 2; break;
        case 0x38: if (regs[AA_].j == 0) pc += S16(pc[1]); else pc += 2; break;
        case 0x39: if (regs[AA_].j != 0) pc += S16(pc[1]); else pc += 2; break;
        case 0x3a: if (regs[AA_].i < 0) pc += S16(pc[1]); else pc += 2; break;
        case 0x3b: if (regs[AA_].i >= 0) pc += S16(pc[1]); else pc += 2; break;
        case 0x3c: if (regs[AA_].i > 0) pc += S16(pc[1]); else pc += 2; break;
        case 0x3d: if (regs[AA_].i <= 0) pc += S16(pc[1]); else pc += 2; break;

#define AGET(op, T, SET) case op: { Obj *a = regs[pc[1] & 0xff].o; int32_t i = regs[pc[1] >> 8].i; if (!a) NPE(); \
            if ((uint32_t)i >= (uint32_t)a->len) THROW("Ljava/lang/ArrayIndexOutOfBoundsException;", "index"); SET(AA_, ((T *)a->data)[i]); pc += 2; break; }
#define APUT(op, T, GET) case op: { Obj *a = regs[pc[1] & 0xff].o; int32_t i = regs[pc[1] >> 8].i; if (!a) NPE(); \
            if ((uint32_t)i >= (uint32_t)a->len) THROW("Ljava/lang/ArrayIndexOutOfBoundsException;", "index"); ((T *)a->data)[i] = (T)GET(AA_); pc += 2; break; }
#define GI(r) (regs[r].i)
#define GJ(r) (regs[r].j)
#define GO(r) (regs[r].o)
        AGET(0x44, int32_t, SETI) AGET(0x45, int64_t, SETJ) AGET(0x46, Obj *, SETO) AGET(0x47, int8_t, SETI)
        AGET(0x48, int8_t, SETI) AGET(0x49, uint16_t, SETI) AGET(0x4a, int16_t, SETI)
        APUT(0x4b, int32_t, GI) APUT(0x4c, int64_t, GJ) APUT(0x4d, Obj *, GO) APUT(0x4e, int8_t, GI)
        APUT(0x4f, int8_t, GI) APUT(0x50, uint16_t, GI) APUT(0x51, int16_t, GI)
        case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: {
            Obj *o = regs[B_].o; if (!o) NPE(); FRef *r = resolve_field(pc[1], 0); regs[A_] = o->f[r->slot]; pc += 2; break; }
        case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f: {
            Obj *o = regs[B_].o; if (!o) NPE(); FRef *r = resolve_field(pc[1], 0); o->f[r->slot] = regs[A_]; pc += 2; break; }
        case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: { FRef *r = resolve_field(pc[1], 1); regs[AA_] = *r->sptr; pc += 2; break; }
        case 0x67: case 0x68: case 0x69: case 0x6a: case 0x6b: case 0x6c: case 0x6d: { FRef *r = resolve_field(pc[1], 1); *r->sptr = regs[AA_]; pc += 2; break; }


        case 0x6e: case 0x6f: case 0x70: case 0x71: case 0x72:
        case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: {
            int op = inst & 0xff, rng = op >= 0x74, kind = rng ? op - 0x74 : op - 0x6e;
            uint16_t midx = pc[1]; Val *av = sp; int n;
            if (rng) { n = AA_; for (int i = 0; i < n; i++) av[i] = regs[pc[2] + i]; }
            else { n = B_; uint16_t r = pc[2]; for (int i = 0; i < n; i++) av[i] = regs[i == 4 ? A_ : (r >> (4 * i)) & 0xf]; }
            pc += 3;
            Method *tm = NULL;
            switch (kind) {
            case 0: case 4: { Obj *r = av[0].o; if (!r) NPE();
                if (((uintptr_t)r & 7) || (uintptr_t)r > 0x7fffffffffffULL || (uintptr_t)r < 0x10000) { fprintf(stderr, "[avm] BAD RECEIVER %p in %s @%04x calling %s\n", (void *)r, m->sig, (unsigned)(ipc - insns), M_DEBUG(midx)); for (int q = cdepth - 1; q >= 0 && q > cdepth - 8; q--) fprintf(stderr, "   at %s\n", cstack[q]->sig); abort(); }
                tm = resolve_virtual(r, midx); break; }
            case 1: {
                if (!av[0].o) NPE();
                uint32_t h = (((uintptr_t)m->cls >> 4) * 31 + midx + 77777) & 8191;
                if (vcache[h].c == m->cls && vcache[h].midx == (midx | 0x10000u)) tm = vcache[h].m;
                else { tm = lookup_chain(m->cls->super, midx, 0); vcache[h].c = m->cls; vcache[h].midx = midx | 0x10000u; vcache[h].m = tm; }
                break; }
            case 2: if (!av[0].o) NPE(); tm = resolve_decl(midx); break;
            default: tm = resolve_decl(midx); if (tm->cls->init_state != 2) { sp = av + n; ensure_init(tm->cls); sp = av; if (g_exc) goto exc; } break;
            }
            if (heap_bytes > gc_next) { sp = av + n; vm_gc(); sp = av; }
            call_method(tm, av, n, &result);
            CHECK();
            break; }

#define UN(op, expr_set) case op: { expr_set; pc++; break; }
        UN(0x7b, SETI(A_, -(uint32_t)regs[B_].i)) UN(0x7c, SETI(A_, ~regs[B_].i))
        UN(0x7d, SETJ(A_, -(uint64_t)regs[B_].j)) UN(0x7e, SETJ(A_, ~regs[B_].j))
        UN(0x7f, SETF(A_, -regs[B_].f)) UN(0x80, SETD(A_, -regs[B_].d))
        UN(0x81, SETJ(A_, (int64_t)regs[B_].i)) UN(0x82, SETF(A_, (float)regs[B_].i)) UN(0x83, SETD(A_, (double)regs[B_].i))
        UN(0x84, SETI(A_, (int32_t)regs[B_].j)) UN(0x85, SETF(A_, (float)regs[B_].j)) UN(0x86, SETD(A_, (double)regs[B_].j))
        UN(0x87, SETI(A_, f2i(regs[B_].f))) UN(0x88, SETJ(A_, f2l(regs[B_].f))) UN(0x89, SETD(A_, (double)regs[B_].f))
        UN(0x8a, SETI(A_, d2i(regs[B_].d))) UN(0x8b, SETJ(A_, f2l(regs[B_].d))) UN(0x8c, SETF(A_, (float)regs[B_].d))
        UN(0x8d, SETI(A_, (int8_t)regs[B_].i)) UN(0x8e, SETI(A_, (uint16_t)regs[B_].i)) UN(0x8f, SETI(A_, (int16_t)regs[B_].i))

#define DOI(k, a, b, dst) do { int32_t x = (a), y = (b); int32_t r_ = 0; \
        switch (k) { case 0: r_ = (int32_t)((uint32_t)x + (uint32_t)y); break; case 1: r_ = (int32_t)((uint32_t)x - (uint32_t)y); break; \
        case 2: r_ = (int32_t)((uint32_t)x * (uint32_t)y); break; \
        case 3: if (!y) THROW("Ljava/lang/ArithmeticException;", "divide by zero"); r_ = y == -1 ? (int32_t)(0u - (uint32_t)x) : x / y; break; \
        case 4: if (!y) THROW("Ljava/lang/ArithmeticException;", "divide by zero"); r_ = y == -1 ? 0 : x % y; break; \
        case 5: r_ = x & y; break; case 6: r_ = x | y; break; case 7: r_ = x ^ y; break; \
        case 8: r_ = (int32_t)((uint32_t)x << (y & 31)); break; case 9: r_ = x >> (y & 31); break; default: r_ = (int32_t)((uint32_t)x >> (y & 31)); } \
        SETI(dst, r_); } while (0)
#define DOL(k, a, b, dst) do { int64_t x = (a), y = (b); int64_t r_ = 0; \
        switch (k) { case 0: r_ = (int64_t)((uint64_t)x + (uint64_t)y); break; case 1: r_ = (int64_t)((uint64_t)x - (uint64_t)y); break; \
        case 2: r_ = (int64_t)((uint64_t)x * (uint64_t)y); break; \
        case 3: if (!y) THROW("Ljava/lang/ArithmeticException;", "divide by zero"); r_ = y == -1 ? (int64_t)(0ull - (uint64_t)x) : x / y; break; \
        case 4: if (!y) THROW("Ljava/lang/ArithmeticException;", "divide by zero"); r_ = y == -1 ? 0 : x % y; break; \
        case 5: r_ = x & y; break; case 6: r_ = x | y; break; case 7: r_ = x ^ y; break; \
        case 8: r_ = (int64_t)((uint64_t)x << (y & 63)); break; case 9: r_ = x >> (y & 63); break; default: r_ = (int64_t)((uint64_t)x >> (y & 63)); } \
        SETJ(dst, r_); } while (0)
#define DOF(k, a, b, dst) do { float x = (a), y = (b), r_ = 0; \
        switch (k) { case 0: r_ = x + y; break; case 1: r_ = x - y; break; case 2: r_ = x * y; break; case 3: r_ = x / y; break; default: r_ = fmodf(x, y); } SETF(dst, r_); } while (0)
#define DOD(k, a, b, dst) do { double x = (a), y = (b), r_ = 0; \
        switch (k) { case 0: r_ = x + y; break; case 1: r_ = x - y; break; case 2: r_ = x * y; break; case 3: r_ = x / y; break; default: r_ = fmod(x, y); } SETD(dst, r_); } while (0)

        case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: case 0x98: case 0x99: case 0x9a: {
            int k = (inst & 0xff) - 0x90; DOI(k, regs[pc[1] & 0xff].i, regs[pc[1] >> 8].i, AA_); pc += 2; break; }
        case 0x9b: case 0x9c: case 0x9d: case 0x9e: case 0x9f: case 0xa0: case 0xa1: case 0xa2: case 0xa3: case 0xa4: case 0xa5: {
            int k = (inst & 0xff) - 0x9b; DOL(k, regs[pc[1] & 0xff].j, (k >= 8 ? (int64_t)regs[pc[1] >> 8].i : regs[pc[1] >> 8].j), AA_); pc += 2; break; }
        case 0xa6: case 0xa7: case 0xa8: case 0xa9: case 0xaa: {
            int k = (inst & 0xff) - 0xa6; DOF(k, regs[pc[1] & 0xff].f, regs[pc[1] >> 8].f, AA_); pc += 2; break; }
        case 0xab: case 0xac: case 0xad: case 0xae: case 0xaf: {
            int k = (inst & 0xff) - 0xab; DOD(k, regs[pc[1] & 0xff].d, regs[pc[1] >> 8].d, AA_); pc += 2; break; }
        case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6: case 0xb7: case 0xb8: case 0xb9: case 0xba: {
            int k = (inst & 0xff) - 0xb0; DOI(k, regs[A_].i, regs[B_].i, A_); pc++; break; }
        case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf: case 0xc0: case 0xc1: case 0xc2: case 0xc3: case 0xc4: case 0xc5: {
            int k = (inst & 0xff) - 0xbb; DOL(k, regs[A_].j, (k >= 8 ? (int64_t)regs[B_].i : regs[B_].j), A_); pc++; break; }
        case 0xc6: case 0xc7: case 0xc8: case 0xc9: case 0xca: {
            int k = (inst & 0xff) - 0xc6; DOF(k, regs[A_].f, regs[B_].f, A_); pc++; break; }
        case 0xcb: case 0xcc: case 0xcd: case 0xce: case 0xcf: {
            int k = (inst & 0xff) - 0xcb; DOD(k, regs[A_].d, regs[B_].d, A_); pc++; break; }
        case 0xd0: case 0xd2: case 0xd3: case 0xd4: case 0xd5: case 0xd6: case 0xd7: {
            int k = (inst & 0xff) - 0xd0; DOI(k, regs[B_].i, S16(pc[1]), A_); pc += 2; break; }
        case 0xd1: SETI(A_, S16(pc[1]) - regs[B_].i); pc += 2; break;
        case 0xd8: case 0xda: case 0xdb: case 0xdc: case 0xdd: case 0xde: case 0xdf: case 0xe0: case 0xe1: case 0xe2: {
            int k = (inst & 0xff) - 0xd8; DOI(k, regs[pc[1] & 0xff].i, (int8_t)(pc[1] >> 8), AA_); pc += 2; break; }
        case 0xd9: SETI(AA_, (int8_t)(pc[1] >> 8) - regs[pc[1] & 0xff].i); pc += 2; break;
        default: vm_fatal("unimplemented opcode 0x%02x in %s at %04x", inst & 0xff, m->sig, (unsigned)(ipc - insns));
        }
        continue;
exc: {
            int h = find_handler(code, (uint32_t)(ipc - insns), g_exc);
            if (h < 0) goto out;
            g_caught = g_exc; g_exc = NULL; pc = insns + h;
        }
    }
out:
    sp = saved_sp; cdepth--;
    (void)isz;
}

/* =========================================================== load / init == */
void vm_init(void) {
    stack = calloc(1 << 19, sizeof(Val)); sp = stack; stack_end = stack + (1 << 19);
    native_class("Ljava/lang/Object;"); native_class("Ljava/lang/String;");
    if (getenv("AVM_TRACE")) vm_trace = atoi(getenv("AVM_TRACE"));
    if (getenv("AVM_GC_KB")) gc_next = (size_t)atol(getenv("AVM_GC_KB")) << 10;
}
int vm_load_dex(const uint8_t *dex, size_t len) {
    if (len < 112 || memcmp(dex, "dex\n", 4)) return -1;
    D = dex; DL = len;
    n_str = rd32(D + 56); p_str = D + rd32(D + 60);
    n_type = rd32(D + 64); p_type = D + rd32(D + 68);
    n_proto = rd32(D + 72); p_proto = D + rd32(D + 76);
    n_field = rd32(D + 80); p_field = D + rd32(D + 84);
    n_meth = rd32(D + 88); p_meth = D + rd32(D + 92);
    n_class = rd32(D + 96); p_class = D + rd32(D + 100);
    tcache = calloc(n_type, sizeof(Class *)); def_of_type = malloc(n_type * sizeof(int));
    for (uint32_t i = 0; i < n_type; i++) def_of_type[i] = -1;
    for (uint32_t i = 0; i < n_class; i++) def_of_type[rd32(p_class + 32 * i)] = i;
    proto_cache = calloc(n_proto, sizeof(char *)); mres = calloc(n_meth, sizeof(Method *));
    fres = calloc(n_field, sizeof(FRef)); g_interned = calloc(n_str, sizeof(Obj *));
    return 0;
}

void vm_dump_stack(void) { for (int i = cdepth - 1; i >= 0 && i > cdepth - 6; i--) fprintf(stderr, "    at %s\n", cstack[i < 256 ? i : 255]->sig); }
void vm_debug_fields(const char *cls) {
    Class *c = vm_class(cls);
    for (Class *k = c; k; k = k->super) {
        printf("class %s ninst=%d nif=%d is_native=%d\n", k->name, k->ninst, k->nif, k->is_native);
        for (int i = 0; i < k->nif; i++) printf("   slot %d  %s : %s\n", k->ifields[i].slot, dstr(k->ifields[i].name_idx), type_name(k->ifields[i].type_idx));
    }
}
