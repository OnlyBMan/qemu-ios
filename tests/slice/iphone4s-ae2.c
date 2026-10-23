/* AE2 and AP exchange independent doorbells; masking must retain pending work.
 * SLICE hw/arm/iphone4s.c fn n94_ae2_update n94_ae2_irq_read n94_ae2_irq_write n94_ae2_timer_expired
 * SLICE hw/arm/iphone4s.c fn n94_ae2_timer_read n94_ae2_timer_write
 * SLICE hw/arm/iphone4s.c fn n94_sac_read n94_sac_write
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef uint64_t hwaddr;
typedef int *qemu_irq;
typedef struct CPU {
    unsigned mp_affinity;
    int irq[2];
} CPU;
static CPU *current_cpu;
#define ARM_CPU(x) (x)
#define DEVICE(x) (x)
#define ARM_CPU_IRQ 0
#define ARM_CPU_FIQ 1
#define HWADDR_PRIx "llx"
#define LOG_GUEST_ERROR 1
#define trace_iphone4s_ae2_control(...) ((void)0)
#define trace_iphone4s_ae2_irq(...) ((void)0)
#define qemu_log_mask(...) ((void)0)
#define QEMU_CLOCK_VIRTUAL 0
#define NANOSECONDS_PER_SECOND 1000000000
static int64_t now = 1000;
typedef struct QEMUTimer {
    int64_t deadline;
    bool armed;
} QEMUTimer;
static int64_t qemu_clock_get_ns(int clock)
{
    return now;
}
static uint64_t muldiv64(uint64_t a, uint64_t b, uint64_t c)
{
    return a * b / c;
}
static void timer_mod(QEMUTimer *timer, int64_t deadline)
{
    timer->deadline = deadline;
    timer->armed = true;
}
static void timer_del(QEMUTimer *timer)
{
    timer->armed = false;
}
static int *qdev_get_gpio_in(CPU *cpu, int n)
{
    return &cpu->irq[n];
}
static void qemu_set_irq(int *irq, int level)
{
    *irq = !!level;
}
typedef struct IPhone4SMachineState {
    uint32_t sac_reg[0x1000 / 4];
    CPU *ae2_cpu;
    qemu_irq ae2_ap_irq[2];
    uint32_t ae2_pending[2], ae2_mask[2], ae2_fiq_mask;
    uint32_t ae2_timer_control, ae2_timer_reload;
    QEMUTimer *ae2_timer;
} IPhone4SMachineState;
typedef struct N94AE2IRQBank {
    IPhone4SMachineState *owner;
    unsigned id;
} N94AE2IRQBank;
#include "slice.h"
int main(void)
{
    CPU ap = {0}, ae2 = {.mp_affinity = 3};
    int ap_irq[2] = {0};
    QEMUTimer timer = {0};
    IPhone4SMachineState s = {.ae2_cpu = &ae2, .ae2_ap_irq = {ap_irq, ap_irq + 1}, .ae2_timer = &timer};
    N94AE2IRQBank ap_bank = {&s, 0}, ae2_bank = {&s, 1};
    N94AE2IRQBank *bank = &ap_bank;
    n94_ae2_irq_write(bank, 8, 0x30, 4);
    n94_ae2_irq_write(bank, 0x20, 0x10, 4);
    assert(s.ae2_pending[1] == 0x10 && !ae2.irq[0] && !ap_irq[0]);
    bank = &ae2_bank;
    n94_ae2_irq_write(bank, 8, 0x10, 4);
    assert(ae2.irq[0] && n94_ae2_irq_read(bank, 4, 4) == 0x10);
    n94_ae2_irq_write(bank, 0xc, 0x10, 4);
    assert(!ae2.irq[0] && s.ae2_pending[1] == 0x10);
    n94_ae2_irq_write(bank, 8, 0x10, 4);
    assert(ae2.irq[0]);
    n94_ae2_irq_write(bank, 0x20, 0x10, 4);
    assert(ap_irq[0] && ae2.irq[0]);
    n94_ae2_irq_write(bank, 0x24, 0x10, 4);
    assert(!ae2.irq[0] && ap_irq[0]);
    bank = &ap_bank;
    n94_ae2_irq_write(bank, 0x24, 0x10, 4);
    assert(!ap_irq[0]);
    n94_ae2_irq_write(bank, 0x20, 0x20, 4);
    bank = &ae2_bank;
    n94_ae2_irq_write(bank, 0x18, 0x20, 4);
    assert(ae2.irq[1] && !ae2.irq[0]);
    n94_ae2_irq_write(bank, 0x24, 0x20, 4);
    assert(!ae2.irq[1]);
    n94_ae2_irq_write(bank, 8, 1, 4);
    n94_ae2_timer_write(&s, 4, 24000, 4);
    n94_ae2_timer_write(&s, 0, 1, 4);
    assert(timer.armed && timer.deadline == now + 1000000 && !ae2.irq[0]);
    n94_ae2_timer_expired(&s);
    assert(ae2.irq[0]);
    n94_ae2_timer_write(&s, 0, 0, 4);
    assert(!ae2.irq[0] && !timer.armed);
    n94_sac_write(&s, 0, 1, 4);
    assert(n94_sac_read(&s, 0, 4) == 0x101);
    n94_sac_write(&s, 0x24, 0x12345678, 4);
    assert(n94_sac_read(&s, 0x24, 4) == 0x12345678);
    n94_sac_write(&s, 0, 0x100, 4);
    assert(n94_sac_read(&s, 0, 4) == 0);
    return 0;
}
