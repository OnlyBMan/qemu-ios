/* A5 AIC routes NAND/IOP interrupts independently of the AP, masks on IACK,
 * and acknowledges the FIQ decrementer separately. Mutation: ignore target[].
 * SLICE hw/arm/s5l8940_aic.c range #define AIC_DESTS | static unsigned aic_current_id
 * SLICE hw/arm/s5l8940_aic.c fn aic_current_id aic_ticks aic_pending_irq aic_update aic_set_irq
 * SLICE hw/arm/s5l8940_aic.c fn aic_timer_expired aic_timer_remaining aic_bank aic_read aic_write aic_reset
 */
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include <string.h>
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define NANOSECONDS_PER_SECOND 1000000000LL
#define QEMU_CLOCK_VIRTUAL 0
#define LOG_UNIMP 0
#define HWADDR_PRIx "llx"
#define qemu_log_mask(...) ((void)0)
typedef uint64_t hwaddr;
typedef int SysBusDevice, MemoryRegion, qemu_irq, DeviceState;
#define S5L8940_AIC(x) ((S5L8940AICState *)(x))
typedef struct {
    int64_t deadline;
    bool pending;
} QEMUTimer;
typedef struct {
    unsigned mp_affinity;
} CPU;
static CPU cpu;
static CPU *current_cpu = &cpu;
#define ARM_CPU(p) (p)
typedef struct S5L8940AICState S5L8940AICState;
static int64_t now;
static int64_t qemu_clock_get_ns(int c)
{
    return now;
}
static uint64_t muldiv64(uint64_t a, uint64_t b, uint64_t c)
{
    return a * b / c;
}
static int ctz32(uint32_t v)
{
    return __builtin_ctz(v);
}
static int levels[8];
static void qemu_set_irq(int irq, int level)
{
    levels[irq] = !!level;
}
static void timer_mod(QEMUTimer *t, int64_t at)
{
    t->deadline = at;
    t->pending = true;
}
static void timer_del(QEMUTimer *t)
{
    t->pending = false;
}
#include "slice.h"
int main(void)
{
    S5L8940AICState s = {0};
    QEMUTimer timer[4] = {0};
    memset(s.mask, 0xff, sizeof(s.mask));
    for (unsigned i = 0; i < 4; i++) {
        s.bank[i].owner = &s;
        s.bank[i].id = i;
        s.bank[i].irq = i * 2;
        s.bank[i].fiq = i * 2 + 1;
        s.bank[i].timer = &timer[i];
        s.bank[i].ipi_mask = ~0u;
        s.bank[i].other_mask = ~0u;
    }
    aic_write(&s, 0x3000 + 33 * 4, 4, 4);
    aic_write(&s, 0x4184, 2, 4);
    aic_set_irq(&s, 33, 1);
    assert(!levels[0] && levels[4]);
    cpu.mp_affinity = 2;
    assert(aic_read(&s, 0x2000, 4) == 2);
    assert(aic_read(&s, 0x2004, 4) == 0x10021);
    assert(!levels[4]);
    aic_set_irq(&s, 33, 0);
    aic_write(&s, 0x4184, 2, 4);
    assert(aic_read(&s, 0x2004, 4) == 0);
    cpu.mp_affinity = 0;
    aic_write(&s, 0x2014, 24000000, 4);
    aic_write(&s, 0x2010, 1, 4);
    assert(timer[0].deadline == 1000000000);
    now = 1000000000;
    aic_timer_expired(&s.bank[0]);
    assert(levels[1] && !levels[5]);
    aic_write(&s, 0x2018, 1, 4);
    assert(!levels[1]);
    aic_write(&s, 0x2028, 1, 4);
    aic_write(&s, 0x2008, 1, 4);
    assert(levels[0] && aic_read(&s, 0x2004, 4) == 0x40001);
    aic_write(&s, 0x200c, 1, 4);
    assert(!levels[0]);
    /* NAND IOP uses the timer's IRQ route, keeping its FIQ reserved for NMI. */
    cpu.mp_affinity = 2;
    aic_write(&s, 0x2014, 24000, 4);
    aic_write(&s, 0x2010, 0xf, 4);
    aic_write(&s, 0x2020, 2, 4);
    aic_timer_expired(&s.bank[2]);
    assert(levels[4] && !levels[5]);
    assert(aic_read(&s, 0x2004, 4) == 0x70001 && !levels[4]);
    aic_write(&s, 0x2010, 0xe, 4);
    aic_write(&s, 0x2018, 1, 4);
    aic_write(&s, 0x2020, 2, 4);
    assert(!levels[4] && !levels[5]);
    aic_write(&s, 0x2010, 0x17, 4);
    aic_write(&s, 0x201c, 1, 4);
    assert(levels[5]);
    aic_write(&s, 0x2010, 0x1f, 4);
    assert(!levels[5]);
    cpu.mp_affinity = 0;
    uint32_t saved = aic_read(&s, 0x4180, 4);
    assert(saved == s.mask[0]);
    aic_reset((DeviceState *)&s);
    aic_write(&s, 0x4100, saved, 4);
    aic_write(&s, 0x4180, ~saved, 4);
    assert(s.mask[0] == saved);
    s.epoch_ns = 123;
    aic_reset((DeviceState *)&s);
    assert(s.epoch_ns == now);
    return 0;
}
