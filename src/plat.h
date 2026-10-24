/* platform abstraction + shared glue between the VM natives and the frontends */
#ifndef PLAT_H
#define PLAT_H
#include <stdint.h>
#include "vm.h"

/* implemented by the frontend (main_linux.c / main_vita.c) */
void        plat_present(const uint32_t *argb, int w, int h);   /* show one finished frame */
void        plat_poll(void);                                    /* poll input, call droid_* below */
void        plat_sleep_ms(int ms);
uint64_t    plat_ms(void);
const char *plat_data_dir(void);                                /* writable dir, trailing '/' */
const char *plat_app_dir(void);                                 /* read-only dir with bundled assets (audio/), trailing '/' */
void        plat_quit(int code);
extern int  plat_log;                                           /* verbose Log.d / println */

/* screen: the game thinks the display is DISP_W x DISP_H (like a 1080p phone) while we
 * render at SCR_W x SCR_H.  Touch input is expected in DISP coordinates. */
#define SCR_W 960
#define SCR_H 540
#define DISP_W 1920
#define DISP_H 1080

/* implemented in droid.c, called by frontends */
void droid_touch(int action, int action_index, int count, const float *xs, const float *ys);
void droid_key(int action, int keycode);
void app_frame_hook(int poll);   /* poll=1 at frame boundaries (input), 0 when merely idling */
void app_thread_start(Obj *thread);

/* implemented in jlib.c, used by droid.c */
Obj *jlib_new_instream(const uint8_t *data, size_t n);
Obj *jlib_new_string_array(char **names, int n);

#define NATIVE(name) static void name(Val *args, Val *ret)
#endif
