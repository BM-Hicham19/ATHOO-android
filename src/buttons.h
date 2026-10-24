/* Maps physical gamepad state onto the game's touch UI (virtual stick + on-screen buttons). */
#ifndef BUTTONS_H
#define BUTTONS_H
/* dir_x/dir_y in [-1,1] (d-pad or analog stick), a/b/c: face buttons, start: pause */
void buttons_update(float dir_x, float dir_y, int act, int swap, int pause, int skip);
#endif
