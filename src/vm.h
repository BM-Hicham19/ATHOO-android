/* avm - a small Dalvik (dex 035) interpreter, just big enough to run the
 * Adventure Time J2ME-style Android game on a PS Vita.  Single-threaded. */
#ifndef AVM_H
#define AVM_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Obj Obj;
typedef struct Class Class;
typedef struct Method Method;

typedef union Val {
    int32_t i; uint32_t u; int64_t j; float f; double d; Obj *o;
} Val;

enum { K_INST = 0, K_ARRAY = 1, K_STRING = 2 };
enum { E_REF = 0, E_1 = 1, E_2 = 2, E_4 = 4, E_8 = 8 };   /* array element size */

struct Obj {
    Class *cls;
    Obj   *next;          /* all-objects list for the GC */
    uint8_t mark, kind, esz, pad;
    int32_t len;          /* array length / string length */
    int32_t nf;           /* number of Val slots in f */
    int32_t cap;          /* capacity for growable natives (Vector) */
    Val    *f;            /* instance / native-visible fields (GC scanned) */
    void   *data;         /* array data, UTF-16 chars, or native payload */
};

typedef void (*NativeFn)(Val *args, Val *ret);

struct Method {
    Class *cls;
    const char *name;
    uint32_t name_idx, proto_idx;
    uint32_t access;
    const uint8_t *code;          /* code_item or NULL */
    int nregs, nins;
    NativeFn native;
    const char *shorty;           /* shorty of proto (static for natives) */
    const char *sig;              /* debug name */
};

typedef struct Field { uint32_t name_idx, type_idx; int slot; } Field;

struct Class {
    const char *name;
    Class *super;
    Class **ifaces; int nifaces;
    int is_native;
    int def_idx;                  /* class_def index or -1 */
    int ninst;                    /* instance slots incl. super */
    Field *ifields; int nif;
    Field *sfields; int nsf; Val *statics;
    Method *methods; int nmeth;
    int init_state;               /* 0 none, 1 running, 2 done */
    int elem_size;                /* arrays */
    int override_native;          /* app class whose listed methods are replaced by natives */
};

/* ---- vm lifecycle ---- */
int  vm_load_dex(const uint8_t *dex, size_t len);
void vm_init(void);
Class *vm_class(const char *name);               /* by descriptor, "Lfoo/Bar;" */
Method *vm_find_method(Class *c, const char *name, const char *protodesc);
void vm_call(Method *m, Val *args, Val *ret);    /* args = nins slots */
int  vm_has_exception(void);
void vm_report_exception(const char *where);
void vm_clear_exception(void);

/* ---- objects ---- */
Obj *vm_new_obj(Class *c);
Obj *vm_new_array(Class *arrcls, int len);
Obj *vm_new_array_t(const char *desc, int len);  /* e.g. "[I" */
Obj *vm_new_string_utf8(const char *s);
Obj *vm_new_string_u16(const uint16_t *c, int n);
char *vm_string_to_utf8(Obj *s);                 /* malloc'd */
Obj *vm_intern_utf8(const char *s);
int  vm_is_instance(Obj *o, Class *c);
void vm_add_root(Obj **slot);
void vm_throw(const char *clsdesc, const char *msg);
void vm_gc(void);
void vm_dump_stack(void);
void vm_note_alloc(size_t n);   /* natives report big malloc'd payloads so the GC paces itself */
#define ARR_I(o)  ((int32_t*)(o)->data)
#define ARR_B(o)  ((int8_t*)(o)->data)
#define ARR_U8(o) ((uint8_t*)(o)->data)
#define ARR_C(o)  ((uint16_t*)(o)->data)
#define ARR_F(o)  ((float*)(o)->data)
#define ARR_R(o)  ((Obj**)(o)->data)
#define STR_C(o)  ((uint16_t*)(o)->data)

/* ---- native registry ---- */
typedef struct NativeDef { const char *sig; NativeFn fn; } NativeDef;
void vm_register_natives(const NativeDef *defs);
typedef struct NativeClassDef { const char *name, *super; const char *fields; /* space separated instance field names */ } NativeClassDef;
void vm_register_native_classes(const NativeClassDef *defs);
typedef struct NativeStaticDef { const char *sig; Obj *(*get)(void); } NativeStaticDef;

/* canonical register writes (keep upper 32 bits sane so refs/ints compare with .j) */
#define RETI(x) (ret->j = (int64_t)(int32_t)(x))
#define RETJ(x) (ret->j = (int64_t)(x))
#define RETO(p) (ret->j = (int64_t)(intptr_t)(p))
#define RETF(x) do { float t_ = (x); ret->j = 0; ret->f = t_; } while (0)
#define RETD(x) (ret->d = (x))
#define SETO_(v,p) ((v).j = (int64_t)(intptr_t)(p))
#define SETI_(v,x) ((v).j = (int64_t)(int32_t)(x))
Val *vm_native_static(const char *sig);   /* lookup of native static fields, may return NULL */
int  vm_native_field_slot(Class *c, const char *name);
void vm_register_statics(const char *sig, Obj *(*mk)(void));

/* helpers used by natives */
Class *vm_cls_string(void);
extern int vm_trace;
void vm_fatal(const char *fmt, ...);
void vm_exit_request(int code);
extern int vm_exit_code, vm_exiting;

/* natives modules */
void jlib_register(void);
void droid_register(void);
void droid_reset(void);
int  droid_boot(const char *apk, const char *main_class);

#endif
