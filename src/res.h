#ifndef RES_H
#define RES_H
#include <stdint.h>
#include <stddef.h>
typedef struct { int w, h; uint32_t *px; } Img;      /* straight ARGB */
uint8_t *inflate_raw(const uint8_t *src, size_t slen, size_t usize_hint, size_t *outlen);
int      zip_open_file(const char *path);
uint8_t *zip_extract(const char *name, size_t *outlen);          /* malloc'd or NULL */
int      zip_exists(const char *name);
int      png_decode(const uint8_t *d, size_t n, Img *out);        /* 0 on success */
int      arsc_init(const uint8_t *d, size_t n);
const char *arsc_get(uint32_t id);                                /* string value or NULL */
#endif
