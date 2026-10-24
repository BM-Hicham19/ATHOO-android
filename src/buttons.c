#include "buttons.h"
#include "input.h"
#include "plat.h"
#include <math.h>

/* Positions measured from the game's own HUD (DISP_W x DISP_H space) */
#define STICK_X 420.0f
#define STICK_Y 760.0f
#define STICK_R 150.0f
#define BTN_ACT_X  1824.0f, 796.0f
#define BTN_SWAP_X 1606.0f, 986.0f
#define BTN_PAUSE_X 1830.0f, 986.0f
#define BTN_SKIP_X 1828.0f, 980.0f

enum { ID_STICK = 20, ID_ACT, ID_SWAP, ID_PAUSE, ID_SKIP };
void buttons_update(float dx, float dy, int act, int swap, int pause, int skip) {
    static int stick_on;
    float m = sqrtf(dx * dx + dy * dy);
    if (m > 0.35f) {
        if (m > 1) { dx /= m; dy /= m; }
        /* the game's stick is "drag relative to where the finger went down": press at the
         * anchor first, then move away from it on the following poll */
        if (!stick_on) { input_set(ID_STICK, 1, STICK_X, STICK_Y); stick_on = 1; }
        else input_set(ID_STICK, 1, STICK_X + dx * STICK_R, STICK_Y + dy * STICK_R);
    } else { stick_on = 0; input_set(ID_STICK, 0, STICK_X, STICK_Y); }
    input_set(ID_ACT,   act,   BTN_ACT_X);
    input_set(ID_SWAP,  swap,  BTN_SWAP_X);
    input_set(ID_PAUSE, pause, BTN_PAUSE_X);
    input_set(ID_SKIP,  skip,  BTN_SKIP_X);
}
