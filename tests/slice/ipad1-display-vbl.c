/* The A4 display's VBL keeps the panel's rate however long the tick's own work takes.
 *
 * vbl_tick latches the frame and paints the console before it re-arms, and QEMU_CLOCK_VIRTUAL runs on during
 * that work (and the timer fires a little late), so arming the next VBL a period after "now" ran the panel at
 * about 50 Hz. It arms a period after the last deadline, and resynchronizes rather than bursting when it is a
 * whole frame behind (a pause, a load).
 *
 * Mutation (named, must fail this test): in vbl_rearm, arm at now + VBL_PERIOD_NS(s) instead of next_vbl.
 *
 * SLICE hw/arm/s5l8930_display.c define VBL_PERIOD_NS[[:space:](]
 * SLICE hw/arm/s5l8930_display.c fn vbl_rearm
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#define NANOSECONDS_PER_SECOND 1000000000LL
#define QEMU_CLOCK_VIRTUAL 1
typedef struct { int64_t expire; } QEMUTimer;
typedef struct { QEMUTimer *vbl; int64_t next_vbl; uint32_t refresh_hz; } S5L8930DisplayState;

static int64_t clock_ns;
static int64_t qemu_clock_get_ns(int type) { return clock_ns; }
static void timer_mod(QEMUTimer *t, int64_t when) { assert(when > clock_ns); t->expire = when; }

#include "slice.h"

/* VBLs in 10 s of virtual time, each tick firing `late` ns after its deadline and working `work` ns. */
static int vbls(uint32_t hz, int64_t late, int64_t work)
{
    QEMUTimer t;
    S5L8930DisplayState s = { &t, 0, hz };
    int n = 0;

    clock_ns = 0;
    s.next_vbl = VBL_PERIOD_NS(&s);   /* as s5l8930_display_reset arms it */
    t.expire = s.next_vbl;
    while (t.expire <= 10 * NANOSECONDS_PER_SECOND) {
        clock_ns = t.expire + late;
        n++;
        clock_ns += work;
        vbl_rearm(&s);
    }
    return n;
}

int main(void)
{
    /* A latch and console paint of 4 ms, the timer 0.3 ms late: still the panel's rate. */
    assert(vbls(60, 0, 0) == 600);
    assert(vbls(60, 300000, 4000000) == 600);
    assert(vbls(120, 300000, 4000000) == 1200);   /* "refresh-hz" */

    /* A whole frame behind (the VM paused for a second): the next VBL is a period away, not a burst. */
    QEMUTimer t;
    S5L8930DisplayState s = { &t, 16666666, 60 };
    clock_ns = 1016666666;
    vbl_rearm(&s);
    assert(t.expire == clock_ns + 16666666);
    puts("PASS: VBL at 60 and 120 Hz with 4 ms of tick work; resynchronizes after a pause");
}
