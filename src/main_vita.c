/* PS Vita frontend: framebuffer, gamepad, touch screen, timing. */
#include <psp2/display.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/power.h>
#include <psp2/audioout.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "plat.h"
#include "input.h"
#include "buttons.h"
#include "audio.h"

#define DATA_DIR  "ux0:data/AdventureTimeVita/"
#define APK_PATH  DATA_DIR "game.apk"
#define MAIN_CLASS "Lcom/globalfun/adventuretime/free/Main;"
#define FB_W 960
#define FB_H 544

int _newlib_heap_size_user = 128 * 1024 * 1024;   /* VM heap, bitmaps */

static void *fbmem[2]; static int fbi;
static int quit_combo;

void plat_sleep_ms(int ms) { sceKernelDelayThread((SceUInt32)ms * 1000u); }
uint64_t plat_ms(void) { return sceKernelGetProcessTimeWide() / 1000; }
const char *plat_data_dir(void) { return DATA_DIR; }
const char *plat_app_dir(void) { return "app0:"; }
void plat_quit(int code) { fflush(stderr); sceKernelExitProcess(code); }

static void fb_alloc(void) {
    for (int i = 0; i < 2; i++) {
        SceUID id = sceKernelAllocMemBlock("fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 0x200000, NULL);
        if (id < 0) { id = sceKernelAllocMemBlock("fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 0x200000, NULL); }
        sceKernelGetMemBlockBase(id, &fbmem[i]);
        memset(fbmem[i], 0, FB_W * FB_H * 4);
    }
}
static void fb_show(void *base) {
    SceDisplayFrameBuf f; memset(&f, 0, sizeof f);
    f.size = sizeof f; f.base = base; f.pitch = FB_W; f.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8; f.width = FB_W; f.height = FB_H;
    sceDisplaySetFrameBuf(&f, SCE_DISPLAY_SETBUF_NEXTFRAME);
}
void plat_present(const uint32_t *argb, int w, int h) {
    uint32_t *dst = (uint32_t *)fbmem[fbi] + (size_t)((FB_H - h) / 2) * FB_W;   /* centre 960x540 in 960x544 */
    size_t n = (size_t)w * h;
    for (size_t i = 0; i < n; i++) { uint32_t p = argb[i]; dst[i] = (p & 0xff00ff00u) | ((p >> 16) & 0xffu) | ((p & 0xffu) << 16); }  /* ARGB -> ABGR */
    fb_show(fbmem[fbi]); fbi ^= 1;
}

static int touch_down[6]; static int touch_id[6];
void plat_poll(void) {
    SceCtrlData pad; memset(&pad, 0, sizeof pad);
    sceCtrlPeekBufferPositive(0, &pad, 1);
    unsigned b = pad.buttons;
    float dx = 0, dy = 0;
    if (b & SCE_CTRL_LEFT) dx -= 1; if (b & SCE_CTRL_RIGHT) dx += 1;
    if (b & SCE_CTRL_UP) dy -= 1;   if (b & SCE_CTRL_DOWN) dy += 1;
    float lx = ((int)pad.lx - 128) / 128.0f, ly = ((int)pad.ly - 128) / 128.0f;
    if (fabsf(lx) > 0.25f || fabsf(ly) > 0.25f) { dx = lx; dy = ly; }
    int act   = (b & (SCE_CTRL_CROSS | SCE_CTRL_CIRCLE | SCE_CTRL_RTRIGGER)) != 0;
    int swap  = (b & (SCE_CTRL_SQUARE | SCE_CTRL_LTRIGGER)) != 0;
    int skip  = (b & SCE_CTRL_TRIANGLE) != 0;
    int pause = (b & SCE_CTRL_START) != 0;
    if ((b & SCE_CTRL_START) && (b & SCE_CTRL_SELECT)) plat_quit(0);        /* Start+Select = quit */
    buttons_update(dx, dy, act, swap, pause, skip);

    SceTouchData td; memset(&td, 0, sizeof td);
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1);
    int seen[6] = {0};
    for (int i = 0; i < (int)td.reportNum && i < 6; i++) {
        int id = td.report[i].id, slot = -1;
        for (int s = 0; s < 6; s++) if (touch_down[s] && touch_id[s] == id) slot = s;
        if (slot < 0) for (int s = 0; s < 6; s++) if (!touch_down[s]) { slot = s; break; }
        if (slot < 0) continue;
        touch_down[slot] = 1; touch_id[slot] = id; seen[slot] = 1;
        input_set(slot, 1, (float)td.report[i].x, (float)td.report[i].y * (DISP_H / 1088.0f));   /* panel is 1920x1088 */
    }
    for (int s = 0; s < 6; s++) if (touch_down[s] && !seen[s]) { touch_down[s] = 0; input_set(s, 0, 0, 0); }
    input_flush();
}

static int audio_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, 1024, AUDIO_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) { fprintf(stderr, "[vita] audio port open failed: 0x%08x\n", port); fflush(stderr); return port; }
    int vol[2] = {SCE_AUDIO_OUT_MAX_VOL, SCE_AUDIO_OUT_MAX_VOL};
    sceAudioOutSetVolume(port, (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), vol);
    static int16_t buf[1024 * 2];
    for (;;) { audio_render(buf, 1024); sceAudioOutOutput(port, buf); }      /* blocks until the buffer has played */
    return 0;
}

int droid_boot(const char *apk, const char *main_class);
static int game_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    int rc = droid_boot(APK_PATH, MAIN_CLASS);
    fprintf(stderr, "[vita] game ended, rc=%d\n", rc); fflush(stderr);
    if (rc) {                                             /* show a red screen for 5 s so a failure is visible */
        for (int f = 0; f < 2; f++) { uint32_t *p = fbmem[f]; for (int i = 0; i < FB_W * FB_H; i++) p[i] = 0xff0000ffu; }
        fb_show(fbmem[0]); sceKernelDelayThread(5 * 1000 * 1000);
    }
    return rc;
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    scePowerSetArmClockFrequency(444); scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222); scePowerSetGpuXbarClockFrequency(166);
    sceIoMkdir("ux0:data/AdventureTimeVita", 0777);
    freopen(DATA_DIR "log.txt", "w", stderr);
    { FILE *f = fopen(DATA_DIR "debug", "rb"); if (f) { plat_log = 1; fclose(f); } }
    fb_alloc(); fb_show(fbmem[0]);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    (void)quit_combo;
    SceUID at = sceKernelCreateThread("audio", audio_thread, 0x40, 0x10000, 0, 0, NULL);
    sceKernelStartThread(at, 0, NULL);
    SceUID th = sceKernelCreateThread("avm", game_thread, 0x40, 2 * 1024 * 1024, 0, 0, NULL);
    sceKernelStartThread(th, 0, NULL);
    int st; sceKernelWaitThreadEnd(th, &st, NULL);
    sceKernelExitProcess(0);
    return 0;
}
