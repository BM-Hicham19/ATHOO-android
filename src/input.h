/* Multi-pointer input layer: frontends set pointer states, input_flush() turns the
 * changes into Android-style MotionEvents (DOWN / POINTER_DOWN / MOVE / POINTER_UP / UP)
 * delivered to the game via droid_touch().  Coordinates are in DISP_W x DISP_H space. */
#ifndef INPUT_H
#define INPUT_H
void input_set(int id, int down, float x, float y);    /* id: any small int (real touches 0..5, virtual 8+) */
void input_flush(void);
#endif
