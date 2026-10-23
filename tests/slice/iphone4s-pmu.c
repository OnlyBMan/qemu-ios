/* D1881's ADC completion is status byte 2 bit 5, cleared when read.
 * Its masks are 0x0e..0x14; 0x15 is standby. Mutation: move status to byte 7.
 * SLICE hw/arm/s5l8940_pmu.c range struct S5L8940D1881State { | static void pmu_update
 * SLICE hw/arm/s5l8940_pmu.c fn pmu_update pmu_adc_done pmu_event pmu_send pmu_recv pmu_reset
 * SLICE hw/arm/s5l8940_pmu.c fn s5l8940_d1881_button s5l8940_d1881_take_suspend
 * SLICE contrib/ios-app/qemu-ios-ui.c fn qemu_ios_ui_guest_shutdown_confirmed
 */
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
typedef struct {
    int dummy;
} I2CSlave, DeviceState;
typedef int qemu_irq;
typedef struct {
    int64_t deadline;
    bool pending;
} QEMUTimer;
typedef struct S5L8940D1881State S5L8940D1881State;
#define S5L8940_D1881(s) ((S5L8940D1881State *)(s))
#define QEMU_CLOCK_VIRTUAL 0
#define SHUTDOWN_CAUSE_GUEST_SHUTDOWN 0
enum i2c_event { I2C_START_SEND };
static int irq, shutdown, ap_level = 1;
#define qatomic_read(p) (*(p))
#define qatomic_set(p, v) (*(p) = (v))
static int ios_is_ipad1;
static bool s5l8930_d1815_guest_shutdown_confirmed(void)
{
    return false;
}
static bool pcf50633_guest_shutdown_confirmed(void)
{
    return false;
}
static void qemu_set_irq(int line, int level)
{
    if (line == 1)
        ap_level = !!level;
    else
        irq = !!level;
}
static int64_t qemu_clock_get_ns(int c)
{
    return 0;
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
static void qemu_system_shutdown_request(int cause)
{
    shutdown++;
}
#include "slice.h"
static void wr(S5L8940D1881State *s, uint8_t reg, uint8_t v)
{
    pmu_event((I2CSlave *)s, I2C_START_SEND);
    pmu_send((I2CSlave *)s, reg);
    pmu_send((I2CSlave *)s, v);
}
static uint8_t rd(S5L8940D1881State *s, uint8_t reg)
{
    pmu_event((I2CSlave *)s, I2C_START_SEND);
    pmu_send((I2CSlave *)s, reg);
    return pmu_recv((I2CSlave *)s);
}
int main(void)
{
    QEMUTimer timer = {0};
    S5L8940D1881State s = {.adc_timer = &timer, .ap_power = 1};
    pmu_reset((DeviceState *)&s);
    assert(!qemu_ios_ui_guest_shutdown_confirmed());
    wr(&s, 0x0f, 0xdf);
    assert(!shutdown);
    wr(&s, 0x43, 0x23);
    assert(timer.pending && timer.deadline == 1000000 && !irq);
    pmu_adc_done(&s);
    assert(irq);
    assert(rd(&s, 2) == 0x20 && !irq);
    assert(rd(&s, 2) == 0);
    assert((rd(&s, 0x44) & 15) + (rd(&s, 0x45) << 4) == 3000);
    wr(&s, 0xaf, 0x80);
    wr(&s, 0x15, 2);
    assert(s.ap_off && !ap_level && !(s.reg[0x15] & 2));
    assert(!qemu_ios_ui_guest_shutdown_confirmed());
    s5l8940_d1881_button((DeviceState *)&s, false, false);
    assert(s.ap_off);
    s5l8940_d1881_button((DeviceState *)&s, false, true);
    assert(!s.ap_off && ap_level && s.ap_waking && (s.reg[1] & 1));
    pmu_reset((DeviceState *)&s);
    assert(s.reg[0xaf] == 0x80 && !s.ap_waking);
    assert(s5l8940_d1881_take_suspend((DeviceState *)&s));
    assert(s.reg[0xaf] == 0x40 && !s5l8940_d1881_take_suspend((DeviceState *)&s));
    wr(&s, 0x15, 1);
    assert(shutdown == 1);
    assert(qemu_ios_ui_guest_shutdown_confirmed());
    pmu_reset((DeviceState *)&s);
    assert(!qemu_ios_ui_guest_shutdown_confirmed());
    return 0;
}
