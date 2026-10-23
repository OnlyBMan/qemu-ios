/* Panel coordinates start at the top; the N1 digitizer counts Y from the bottom.
 * SLICE hw/arm/iphone4s.c fn n94_mouse_event
 */
#include <assert.h>
#include <stdbool.h>
#include <math.h>
typedef struct IPodTouchMultitouchState {
    float touch_x, touch_y;
    bool touch_down;
} IPodTouchMultitouchState;
static int starts, moves, ends;
static void ipod_touch_multitouch_on_touch(IPodTouchMultitouchState *s)
{
    s->touch_down = true;
    starts++;
}
static void ipod_touch_multitouch_on_release(IPodTouchMultitouchState *s)
{
    s->touch_down = false;
    ends++;
}
static void ipod_touch_multitouch_on_motion(IPodTouchMultitouchState *s)
{
    moves++;
}
#include "slice.h"
int main(void)
{
    IPodTouchMultitouchState s = {0};
    n94_mouse_event(&s, 0, 0, 0, 1);
    assert(starts == 1 && s.touch_x == 0 && s.touch_y == 1);
    n94_mouse_event(&s, 8192, 24576, 0, 1);
    assert(moves == 1 && s.touch_x == .25f && s.touch_y == .25f);
    n94_mouse_event(&s, 32767, 32767, 0, 0);
    assert(ends == 1 && !s.touch_down && s.touch_x > .999f && s.touch_y < .001f);
    n94_mouse_event(&s, 0, 0, 0, 0);
    assert(ends == 1 && starts == 1 && moves == 1);
    return 0;
}
