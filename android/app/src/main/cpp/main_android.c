/* Android frontend (NativeActivity): display, touch + gamepad input, audio, asset unpacking.
 * The game runs on this app thread; android events are pumped from inside plat_poll()/plat_present(). */
#include <android_native_app_glue.h>
#include <android/log.h>
#include <android/asset_manager.h>
#include <android/native_window.h>
#include <aaudio/AAudio.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "plat.h"
#include "input.h"
#include "buttons.h"
#include "audio.h"

#define MAIN_CLASS "Lcom/globalfun/adventuretime/free/Main;"
#define MAXT 10

static struct android_app *g_app;
static ANativeWindow *g_win;
static int g_destroy, g_paused;
static char g_data[600];

/* letterbox geometry (screen pixels -> game "DISP" coordinates) */
static int g_geom_ok, g_clear, g_bw, g_bh, g_offx, g_offy, g_sw, g_sh;

/* ------------------------------------------------------------ platform glue -- */
void plat_sleep_ms(int ms) { if (ms > 0) usleep((useconds_t)ms * 1000u); }
uint64_t plat_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000u + t.tv_nsec / 1000000u; }
const char *plat_data_dir(void) { return g_data; }
const char *plat_app_dir(void) { return g_data; }
void plat_quit(int code) {
    fflush(stderr);
    if (g_app) ANativeActivity_finish(g_app->activity);
    exit(code);
}

/* log: everything the game writes to stderr goes to logcat and to <data>/log.txt */
static void *log_thread(void *arg) {
    FILE *in = fdopen((int)(intptr_t)arg, "r"); char path[700]; snprintf(path, sizeof path, "%slog.txt", g_data);
    FILE *lf = fopen(path, "w"); char line[1024];
    while (in && fgets(line, sizeof line, in)) { __android_log_write(ANDROID_LOG_INFO, "avm", line); if (lf) { fputs(line, lf); fflush(lf); } }
    return NULL;
}
static void start_log(void) {
    int p[2]; if (pipe(p)) return; dup2(p[1], 2); setvbuf(stderr, NULL, _IONBF, 0);
    pthread_t t; pthread_create(&t, NULL, log_thread, (void *)(intptr_t)p[0]); pthread_detach(t);
}

/* -------------------------------------------------------------- event loop -- */
static AAudioStream *g_astream;
static void audio_pause(int pause) {
    if (!g_astream) return;
    if (pause) AAudioStream_requestPause(g_astream); else AAudioStream_requestStart(g_astream);
}
static void on_cmd(struct android_app *app, int32_t cmd) {
    switch (cmd) {
        case APP_CMD_INIT_WINDOW: g_win = app->window; g_geom_ok = 0; break;
        case APP_CMD_TERM_WINDOW: g_win = NULL; g_geom_ok = 0; break;
        case APP_CMD_PAUSE: g_paused = 1; audio_pause(1); break;
        case APP_CMD_RESUME: g_paused = 0; audio_pause(0); break;
        case APP_CMD_DESTROY: g_destroy = 1; break;
        default: break;
    }
}

/* gamepad / keyboard state */
static int k_up, k_down, k_left, k_right, k_act, k_swap, k_skip, k_pause;
static float stick_x, stick_y;
static float tx_last[MAXT], ty_last[MAXT]; static int t_down[MAXT];

/* screen pixel -> game DISP coordinates (1920x1080 space), through the letterbox */
static void map_touch(float x, float y, float *dx, float *dy) {
    float bx = g_sw > 0 ? x * g_bw / g_sw : x, by = g_sh > 0 ? y * g_bh / g_sh : y;
    float cx = (bx - g_offx) * (DISP_W / (float)SCR_W), cy = (by - g_offy) * (DISP_H / (float)SCR_H);
    if (cx < 0) cx = 0; if (cx > DISP_W - 1) cx = DISP_W - 1;
    if (cy < 0) cy = 0; if (cy > DISP_H - 1) cy = DISP_H - 1;
    *dx = cx; *dy = cy;
}
static int32_t on_input(struct android_app *app, AInputEvent *e) {
    int type = AInputEvent_getType(e);
    if (type == AINPUT_EVENT_TYPE_MOTION) {
        int src = AInputEvent_getSource(e);
        if (src & AINPUT_SOURCE_JOYSTICK) {                      /* analogue stick / hat of a gamepad */
            stick_x = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_X, 0) + AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_X, 0);
            stick_y = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Y, 0) + AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_Y, 0);
            return 1;
        }
        int32_t act = AMotionEvent_getAction(e); int masked = act & AMOTION_EVENT_ACTION_MASK;
        int idx = (act & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
        size_t n = AMotionEvent_getPointerCount(e); int seen[MAXT] = {0};
        for (size_t i = 0; i < n; i++) {
            int id = AMotionEvent_getPointerId(e, i); if (id < 0 || id >= MAXT) continue;
            float dx, dy; map_touch(AMotionEvent_getX(e, i), AMotionEvent_getY(e, i), &dx, &dy);
            int up = masked == AMOTION_EVENT_ACTION_UP || masked == AMOTION_EVENT_ACTION_CANCEL || (masked == AMOTION_EVENT_ACTION_POINTER_UP && (int)i == idx);
            if (up) { input_set(id, 0, dx, dy); t_down[id] = 0; }
            else { input_set(id, 1, dx, dy); t_down[id] = 1; seen[id] = 1; }
            tx_last[id] = dx; ty_last[id] = dy;
        }
        for (int id = 0; id < MAXT; id++) if (t_down[id] && !seen[id]) { input_set(id, 0, tx_last[id], ty_last[id]); t_down[id] = 0; }
        return 1;
    }
    if (type == AINPUT_EVENT_TYPE_KEY) {
        int code = AKeyEvent_getKeyCode(e), down = AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_DOWN;
        switch (code) {
            case AKEYCODE_DPAD_UP: k_up = down; return 1;       case AKEYCODE_DPAD_DOWN: k_down = down; return 1;
            case AKEYCODE_DPAD_LEFT: k_left = down; return 1;   case AKEYCODE_DPAD_RIGHT: k_right = down; return 1;
            case AKEYCODE_BUTTON_A: case AKEYCODE_BUTTON_B: case AKEYCODE_BUTTON_R1: case AKEYCODE_DPAD_CENTER: k_act = down; return 1;
            case AKEYCODE_BUTTON_X: case AKEYCODE_BUTTON_L1: k_swap = down; return 1;
            case AKEYCODE_BUTTON_Y: k_skip = down; return 1;
            case AKEYCODE_BUTTON_START: k_pause = down; return 1;
            default: return 0;                                   /* BACK etc.: system default (closes the app) */
        }
    }
    return 0;
}
static void pump(int timeout_ms) {
    int ev; struct android_poll_source *src;
    while (ALooper_pollOnce(timeout_ms, NULL, &ev, (void **)&src) >= 0) {
        if (src) src->process(g_app, src);
        timeout_ms = 0;
        if (g_app->destroyRequested) g_destroy = 1;
    }
}
void plat_poll(void) {
    pump(0);
    if (g_destroy) plat_quit(0);
    float dx = stick_x, dy = stick_y;
    if (k_left) dx -= 1; if (k_right) dx += 1; if (k_up) dy -= 1; if (k_down) dy += 1;
    buttons_update(dx, dy, k_act, k_swap, k_pause, k_skip);
    input_flush();
}

/* ----------------------------------------------------------------- display -- */
static void setup_geometry(void) {
    g_sw = ANativeWindow_getWidth(g_win); g_sh = ANativeWindow_getHeight(g_win);
    if (g_sw <= 0 || g_sh <= 0) { g_sw = 16; g_sh = 9; }
    if ((long)g_sw * 9 >= (long)g_sh * 16) { g_bh = SCR_H; g_bw = (int)(SCR_H * (double)g_sw / g_sh + 0.5); }     /* wider than 16:9 -> side bars */
    else { g_bw = SCR_W; g_bh = (int)(SCR_W * (double)g_sh / g_sw + 0.5); }                                        /* taller -> top/bottom bars */
    if (g_bw < SCR_W) g_bw = SCR_W; if (g_bh < SCR_H) g_bh = SCR_H;
    g_offx = (g_bw - SCR_W) / 2; g_offy = (g_bh - SCR_H) / 2;
    ANativeWindow_setBuffersGeometry(g_win, g_bw, g_bh, WINDOW_FORMAT_RGBA_8888);
    g_geom_ok = 1; g_clear = 4;
}
void plat_present(const uint32_t *argb, int w, int h) {
    pump(0);
    while ((!g_win || g_paused) && !g_destroy) pump(-1);      /* backgrounded: freeze the game until we're back */
    if (g_destroy) plat_quit(0);
    if (!g_geom_ok) setup_geometry();
    ANativeWindow_Buffer b;
    if (ANativeWindow_lock(g_win, &b, NULL) < 0) return;
    uint32_t *dst = (uint32_t *)b.bits;
    if (g_clear > 0) { for (int y = 0; y < b.height; y++) { uint32_t *r = dst + (size_t)y * b.stride; for (int x = 0; x < b.width; x++) r[x] = 0xff000000u; } g_clear--; }
    for (int y = 0; y < h && y + g_offy < b.height; y++) {
        const uint32_t *s = argb + (size_t)y * w; uint32_t *d = dst + (size_t)(y + g_offy) * b.stride + g_offx;
        for (int x = 0; x < w && x + g_offx < b.width; x++) { uint32_t p = s[x]; d[x] = (p & 0xff00ff00u) | ((p >> 16) & 0xffu) | ((p & 0xffu) << 16); }   /* ARGB -> ABGR (RGBA bytes) */
    }
    ANativeWindow_unlockAndPost(g_win);
}

/* ------------------------------------------------------------------- audio -- */
static aaudio_data_callback_result_t audio_cb(AAudioStream *s, void *ud, void *data, int32_t frames) {
    int16_t *o = (int16_t *)data;
    while (frames > 0) { int n = frames > 1024 ? 1024 : frames; audio_render(o, n); o += n * 2; frames -= n; }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
static void audio_start(void) {
    AAudioStreamBuilder *b; if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) { fprintf(stderr, "[android] AAudio unavailable\n"); return; }
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSampleRate(b, AUDIO_RATE);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setDataCallback(b, audio_cb, NULL);
    if (AAudioStreamBuilder_openStream(b, &g_astream) == AAUDIO_OK) AAudioStream_requestStart(g_astream);
    else fprintf(stderr, "[android] could not open audio stream\n");
    AAudioStreamBuilder_delete(b);
}

/* ------------------------------------------------------------------ assets -- */
static int copy_asset(const char *name, const char *dst) {
    AAsset *a = AAssetManager_open(g_app->activity->assetManager, name, AASSET_MODE_STREAMING); if (!a) return -1;
    off_t len = AAsset_getLength(a); struct stat st;
    if (stat(dst, &st) == 0 && st.st_size == len) { AAsset_close(a); return 0; }          /* already unpacked */
    FILE *f = fopen(dst, "wb"); if (!f) { AAsset_close(a); return -2; }
    static char buf[65536]; int n; while ((n = AAsset_read(a, buf, sizeof buf)) > 0) fwrite(buf, 1, n, f);
    fclose(f); AAsset_close(a); return 0;
}
static void unpack_audio(void) {
    char dir[700]; snprintf(dir, sizeof dir, "%saudio", g_data); mkdir(dir, 0755);
    AAssetDir *d = AAssetManager_openDir(g_app->activity->assetManager, "audio"); if (!d) return;
    const char *nm; while ((nm = AAssetDir_getNextFileName(d))) {
        char src[300], dst[800]; snprintf(src, sizeof src, "audio/%s", nm); snprintf(dst, sizeof dst, "%saudio/%s", g_data, nm); copy_asset(src, dst);
    }
    AAssetDir_close(d);
}
static void show_error_screen(void) {                         /* solid red for a few seconds so a failure is visible */
    static uint32_t red[SCR_W * SCR_H]; for (int i = 0; i < SCR_W * SCR_H; i++) red[i] = 0xffe02020u;
    for (int i = 0; i < 60; i++) { plat_present(red, SCR_W, SCR_H); plat_sleep_ms(100); }
}

int droid_boot(const char *apk, const char *main_class);
void android_main(struct android_app *app) {
    g_app = app; app->onAppCmd = on_cmd; app->onInputEvent = on_input;
    snprintf(g_data, sizeof g_data, "%s/", app->activity->internalDataPath);
    start_log();
    fprintf(stderr, "[android] starting, data dir %s\n", g_data);
    while (!g_win && !g_destroy) pump(-1);                     /* wait for the surface */
    if (g_destroy) return;
    char apk[700]; snprintf(apk, sizeof apk, "%sgame.apk", g_data);
    int rc = copy_asset("game.apk", apk);
    if (rc) { fprintf(stderr, "[android] game.apk is missing from the app (rc=%d): put your APK in android/app/src/main/assets/game.apk and rebuild\n", rc); show_error_screen(); plat_quit(1); }
    unpack_audio(); audio_start();
    rc = droid_boot(apk, MAIN_CLASS);
    fprintf(stderr, "[android] game ended, rc=%d\n", rc);
    if (rc) show_error_screen();
    plat_quit(rc);
}
