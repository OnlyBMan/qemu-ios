/* Dialog D1881 PMU on the N94 i2c0 bus, address 0x74. Its IRQ mask
 * bank is 0x0e..0x14 (10B329 AppleD1881PMU::start, 80aea3dc), unlike
 * D1815 where those bytes include system power control. Keep separate
 * register files: sharing the D1815 model turns mask initialization into
 * an unintended host-side power-off.
 */
#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/arm/s5l8930.h"
#include "hw/core/irq.h"
#include "system/runstate.h"
#include "qom/object.h"
#include "qemu/timer.h"
#include "migration/vmstate.h"

#define TYPE_S5L8940_D1881 "s5l8940.d1881"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8940D1881State, S5L8940_D1881)
struct S5L8940D1881State {
    I2CSlave parent_obj;
    uint8_t reg[256], address;
    bool address_pending;
    qemu_irq irq, ap_power;
    bool ap_off, ap_waking;
    QEMUTimer *adc_timer;
};
// Read by the app thread after the guest unmounts its volumes and enters standby.
static int d1881_shutdown_confirmed;
bool s5l8940_d1881_guest_shutdown_confirmed(void)
{
    return qatomic_read(&d1881_shutdown_confirmed);
}
static void pmu_update(S5L8940D1881State *s)
{
    unsigned pending = 0;
    for (unsigned i = 0; i < 7; i++) {
        pending |= s->reg[1 + i] & ~s->reg[14 + i];
    }
    qemu_set_irq(s->irq, pending != 0);
}
void s5l8940_d1881_button(DeviceState *dev, bool hold, bool down)
{
    S5L8940D1881State *s = S5L8940_D1881(dev);
    if (!down)
        return;
    s->reg[1] |= hold ? 2 : 1;
    pmu_update(s);
    if (s->ap_off) {
        s->ap_off = false;
        s->ap_waking = true;
        qemu_set_irq(s->ap_power, 1);
    }
}

bool s5l8940_d1881_take_suspend(DeviceState *dev)
{
    uint8_t *reason = &S5L8940_D1881(dev)->reg[0xaf];
    if ((*reason & 0xd0) != 0x80)
        return false;
    *reason = (*reason & 0x2f) | 0x40;
    return true;
}

static void pmu_adc_done(void *opaque)
{
    S5L8940D1881State *s = opaque;
    unsigned channel = s->reg[0x43] & 0x1f;
    unsigned sample = channel == 3 ? 3000 : 2048;
    s->reg[0x44] = sample & 0xf;
    s->reg[0x45] = sample >> 4;
    s->reg[2] |= 0x20;
    pmu_update(s);
}
static int pmu_event(I2CSlave *dev, enum i2c_event event)
{
    S5L8940D1881State *s = S5L8940_D1881(dev);
    if (event == I2C_START_SEND) {
        s->address_pending = true;
    }
    return 0;
}
static int pmu_send(I2CSlave *dev, uint8_t value)
{
    S5L8940D1881State *s = S5L8940_D1881(dev);
    if (s->address_pending) {
        s->address = value;
        s->address_pending = false;
    } else {
        unsigned reg = s->address++ & 0xff;
        if (reg >= 1 && reg < 8) {
            s->reg[reg] &= ~value;
        } else {
            s->reg[reg] = value;
        }
        if (reg == 0x43 && (value & 0x20)) {
            timer_mod(s->adc_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
        }
        if (reg == 0x15) {
            if (value & 1) {
                qatomic_set(&d1881_shutdown_confirmed, 1);
                qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
            } else if (value & 2) {
                s->reg[reg] &= ~2u;
                s->ap_off = true;
                qemu_set_irq(s->ap_power, 0);
            }
        }
        pmu_update(s);
    }
    return 0;
}
static uint8_t pmu_recv(I2CSlave *dev)
{
    S5L8940D1881State *s = S5L8940_D1881(dev);
    unsigned reg = s->address++ & 0xff;
    uint8_t value = s->reg[reg];
    if (reg >= 1 && reg < 8) {
        s->reg[reg] = 0;
        pmu_update(s);
    }
    return value;
}
static void pmu_reset(DeviceState *dev)
{
    S5L8940D1881State *s = S5L8940_D1881(dev);
    if (s->ap_waking) {
        s->ap_waking = false;
        return;
    }
    qatomic_set(&d1881_shutdown_confirmed, 0);
    s->ap_off = false;
    timer_del(s->adc_timer);
    memset(s->reg, 0, sizeof(s->reg));
    memset(s->reg + 14, 0xff, 7);
    s->address = 0;
    s->address_pending = true;
    pmu_update(s);
}
static void pmu_init(Object *obj)
{
    S5L8940_D1881(obj)->adc_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, pmu_adc_done, obj);
    qdev_init_gpio_out(DEVICE(obj), &S5L8940_D1881(obj)->irq, 1);
    qdev_init_gpio_out_named(DEVICE(obj), &S5L8940_D1881(obj)->ap_power, "ap-power", 1);
}
static int pmu_post_load(void *opaque, int version)
{
    pmu_update(opaque);
    return 0;
}
static const VMStateDescription vmstate_pmu = {
    .name = TYPE_S5L8940_D1881,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = pmu_post_load,
    .fields = (const VMStateField[]){
        VMSTATE_I2C_SLAVE(parent_obj, S5L8940D1881State), VMSTATE_UINT8_ARRAY(reg, S5L8940D1881State, 256),
        VMSTATE_BOOL(ap_off, S5L8940D1881State), VMSTATE_BOOL(ap_waking, S5L8940D1881State),
        VMSTATE_UINT8(address, S5L8940D1881State), VMSTATE_BOOL(address_pending, S5L8940D1881State),
        VMSTATE_TIMER_PTR(adc_timer, S5L8940D1881State), VMSTATE_END_OF_LIST()}};
static void pmu_finalize(Object *obj)
{
    timer_free(S5L8940_D1881(obj)->adc_timer);
}
static void pmu_class_init(ObjectClass *oc, const void *data)
{
    I2CSlaveClass *ic = I2C_SLAVE_CLASS(oc);
    ic->event = pmu_event;
    ic->send = pmu_send;
    ic->recv = pmu_recv;
    device_class_set_legacy_reset(DEVICE_CLASS(oc), pmu_reset);
    DEVICE_CLASS(oc)->vmsd = &vmstate_pmu;
}
static const TypeInfo pmu_type = {
    .name = TYPE_S5L8940_D1881,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(S5L8940D1881State),
    .instance_init = pmu_init,
    .class_init = pmu_class_init,
    .instance_finalize = pmu_finalize,
};
static void pmu_register_types(void)
{
    type_register_static(&pmu_type);
}
type_init(pmu_register_types)
