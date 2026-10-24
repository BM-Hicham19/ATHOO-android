#include "input.h"
#include "plat.h"
#include <string.h>
#include <math.h>

#define MAXP 10
typedef struct { int id, down; float x, y; } PS;
static PS want[MAXP + 4];  static int nwant;       /* requested state, any id */
static PS rep[MAXP];       static int nrep;        /* what the game has been told, in slot order */

static int emitted;
static PS *find_want(int id) { for (int i = 0; i < nwant; i++) if (want[i].id == id) return &want[i]; return NULL; }
void input_set(int id, int down, float x, float y) {
    PS *p = find_want(id);
    if (!p) { if (!down || nwant >= MAXP) return; p = &want[nwant++]; p->id = id; p->down = 0; }
    p->down = down; p->x = x; p->y = y;
}
static void emit(int action, int idx) {
    emitted = 1;
    float xs[6], ys[6]; int n = nrep > 6 ? 6 : nrep;
    for (int i = 0; i < n; i++) { xs[i] = rep[i].x; ys[i] = rep[i].y; }
    droid_touch(action, idx, n, xs, ys);
}
void input_flush(void) {
    emitted = 0;
    /* 1. releases */
    for (int i = 0; i < nrep && !emitted; ) {
        PS *w = find_want(rep[i].id);
        if (!w || !w->down) {
            if (w) { rep[i].x = w->x; rep[i].y = w->y; }
            emit(nrep == 1 ? 1 : 6, i);
            memmove(&rep[i], &rep[i + 1], (nrep - i - 1) * sizeof(PS)); nrep--;
        } else i++;
    }
    /* 2. presses */
    for (int k = 0; k < nwant && !emitted; k++) {
        PS *w = &want[k]; if (!w->down) continue;
        int known = 0; for (int i = 0; i < nrep; i++) if (rep[i].id == w->id) known = 1;
        if (known || nrep >= 6) continue;
        rep[nrep] = *w; nrep++;
        emit(nrep == 1 ? 0 : 5, nrep - 1);
    }
    /* 3. moves */
    int moved = 0;
    for (int i = 0; i < nrep && !emitted; i++) { PS *w = find_want(rep[i].id); if (w && (fabsf(w->x - rep[i].x) > 0.5f || fabsf(w->y - rep[i].y) > 0.5f)) { rep[i].x = w->x; rep[i].y = w->y; moved = 1; } }
    if (moved) emit(2, 0);
    /* forget pointers that are up and no longer reported */
    for (int k = 0; k < nwant; ) {
        int still = 0; for (int i = 0; i < nrep; i++) if (rep[i].id == want[k].id) still = 1;
        if (!want[k].down && !still) { memmove(&want[k], &want[k + 1], (nwant - k - 1) * sizeof(PS)); nwant--; } else k++;
    }
}
