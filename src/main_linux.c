/* Linux test frontend: runs the game headless, dumps frames as PPM, scripted touches. */
#include "plat.h"
#include <time.h>
#include "input.h"
#include "buttons.h"
#include "audio.h"
#include <sys/stat.h>

static long frames, max_frames = 300, dump_every = 100;
static uint64_t vtime = 1000000;
static const char *outdir = "/tmp/atframes";
static char touchscript[1024];

static FILE *aud; static double acarry; static long afr;
static void aud_close(void) {
    if (!aud) return; uint32_t dl = afr * 4, rl = 36 + dl; fseek(aud, 4, SEEK_SET); fwrite(&rl, 4, 1, aud); fseek(aud, 40, SEEK_SET); fwrite(&dl, 4, 1, aud); fclose(aud); aud = NULL;
}
static void aud_open(const char *p) {
    aud = fopen(p, "wb"); if (!aud) return;
    uint32_t z = 0, fl = 16, rate = AUDIO_RATE, br = rate * 4; uint16_t fmt = 1, ch = 2, ba = 4, bits = 16;
    fwrite("RIFF", 1, 4, aud); fwrite(&z, 4, 1, aud); fwrite("WAVEfmt ", 1, 8, aud); fwrite(&fl, 4, 1, aud); fwrite(&fmt, 2, 1, aud); fwrite(&ch, 2, 1, aud);
    fwrite(&rate, 4, 1, aud); fwrite(&br, 4, 1, aud); fwrite(&ba, 2, 1, aud); fwrite(&bits, 2, 1, aud); fwrite("data", 1, 4, aud); fwrite(&z, 4, 1, aud); atexit(aud_close);
}
static long nsleep, msleep;
void plat_sleep_ms(int ms) {
    vtime += ms; nsleep++; msleep += ms;
    if (aud) { acarry += ms * (AUDIO_RATE / 1000.0); int n = (int)acarry; acarry -= n; while (n > 0) { int k = n > 1024 ? 1024 : n; int16_t b[2048]; audio_render(b, k); fwrite(b, 4, k, aud); afr += k; n -= k; } }
}
uint64_t plat_ms(void) { return vtime; }
const char *plat_data_dir(void) { return "/tmp/atdata/"; }
const char *plat_app_dir(void) { static char b[512]; const char *e = getenv("AVM_APPDIR"); snprintf(b, sizeof b, "%s%s", e ? e : "./", (e && e[0] && e[strlen(e) - 1] != '/') ? "/" : ""); return b; }
void plat_quit(int code) { fprintf(stderr, "[plat] quit(%d) after %ld frames\n", code, frames); exit(code); }

static void dump(const uint32_t *px, int w, int h, const char *name) {
    char p[256]; snprintf(p, sizeof p, "%s/%s.ppm", outdir, name);
    FILE *f = fopen(p, "wb"); if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) { uint8_t c[3] = {px[i] >> 16, px[i] >> 8, px[i]}; fwrite(c, 1, 3, f); }
    fclose(f);
}
void plat_present(const uint32_t *px, int w, int h) {
    frames++;
    if (dump_every && frames % dump_every == 0) { char n[32]; snprintf(n, sizeof n, "f%05ld", frames); dump(px, w, h, n); }
    if (frames >= max_frames) { dump(px, w, h, "final"); { FILE *f = fopen("/proc/self/status", "r"); char l[256]; while (f && fgets(l, sizeof l, f)) if (!strncmp(l, "VmHWM", 5) || !strncmp(l, "VmRSS", 5)) fprintf(stderr, "[mem] %s", l); if (f) fclose(f); } fprintf(stderr, "[plat] reached %ld frames; sleeps=%ld total=%ld ms (virtual clock %llu)\n", frames, nsleep, msleep, (unsigned long long)vtime); exit(0); }
}
static char padscript[1024];
void plat_poll(void) {
    /* touch script: "frame:x:y;..." -> tap held for 3 frames; pad script: "f0-f1:dx:dy:act;..." */
    const char *s = touchscript; int k = 0;
    while (*s) {
        long f; float x, y; int used = 0;
        if (sscanf(s, "%ld:%f:%f%n", &f, &x, &y, &used) < 3) break;
        if (frames >= f && frames < f + 3) input_set(50 + k, 1, x, y); else if (frames == f + 3) input_set(50 + k, 0, x, y);
        s += used; while (*s == ';') s++; k++;
    }
    float dx = 0, dy = 0; int act = 0; s = padscript;
    while (*s) {
        long a, b; float x, y; int ac = 0, used = 0;
        if (sscanf(s, "%ld-%ld:%f:%f:%d%n", &a, &b, &x, &y, &ac, &used) < 5) break;
        if (frames >= a && frames <= b) { dx = x; dy = y; act = ac; }
        s += used; while (*s == ';') s++;
    }
    buttons_update(dx, dy, act, 0, 0, 0);
    input_flush();
}
int droid_boot(const char *apk, const char *main_class);
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s game.apk [frames] [dump_every] [touchscript]\n", argv[0]); return 1; }
    if (argc > 2) max_frames = atol(argv[2]);
    if (argc > 3) dump_every = atol(argv[3]);
    if (argc > 4) snprintf(touchscript, sizeof touchscript, "%s", argv[4]);
    if (argc > 5) snprintf(padscript, sizeof padscript, "%s", argv[5]);
    if (getenv("AVM_LOG")) plat_log = 1;
    if (getenv("AVM_AUDIO_OUT")) aud_open(getenv("AVM_AUDIO_OUT"));
    mkdir(outdir, 0755); mkdir("/tmp/atdata", 0755);
    return droid_boot(argv[1], "Lcom/globalfun/adventuretime/free/Main;");
}
