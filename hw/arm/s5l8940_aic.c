/* First-generation Apple AIC: 192 external IRQs and four destinations
 * (AP0, AP1, IOP, AE2). N94 10B329 contracts: PE at 80087b20/80087b60,
 * AppleInterruptController at 80b06224/80b06594/80b06964. The 24 MHz
 * counter is at +0x20/+0x28; each destination has a decrementer with
 * W1C timer status at +0x2018, independent of the IRQ IACK register.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "target/arm/cpu.h"
#include "qom/object.h"
#include "migration/vmstate.h"

#define TYPE_S5L8940_AIC "s5l8940.aic"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8940AICState, S5L8940_AIC)
#define AIC_DESTS 4
#define AIC_EXT_IRQS 192
#define AIC_WORDS (AIC_EXT_IRQS / 32)
#define AIC_HZ 24000000

typedef struct AICBank {
    S5L8940AICState *owner;
    unsigned id;
    qemu_irq irq, fiq;
    QEMUTimer *timer;
    int64_t deadline_ns;
    uint32_t control, reload, ipi_pending, ipi_mask, other_mask;
    bool nmi_pending;
    bool timer_pending;
} AICBank;

struct S5L8940AICState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    int64_t epoch_ns;
    uint32_t config;
    uint32_t levels[AIC_WORDS], software[AIC_WORDS], mask[AIC_WORDS];
    uint32_t target[AIC_EXT_IRQS];
    AICBank bank[AIC_DESTS];
};

static unsigned aic_current_id(void)
{
    return current_cpu ? ARM_CPU(current_cpu)->mp_affinity & 3 : 0;
}

static uint64_t aic_ticks(S5L8940AICState *s)
{
    return muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->epoch_ns, AIC_HZ, NANOSECONDS_PER_SECOND);
}

static int aic_pending_irq(S5L8940AICState *s, unsigned id)
{
    for (int i = 0; i < AIC_WORDS; i++) {
        uint32_t bits = (s->levels[i] | s->software[i]) & ~s->mask[i];
        while (bits) {
            int bit = ctz32(bits);
            int irq = i * 32 + bit;
            if (irq < AIC_EXT_IRQS && (s->target[irq] & (1u << id))) {
                return irq;
            }
            bits &= ~(1u << bit);
        }
    }
    return -1;
}

static void aic_update(S5L8940AICState *s)
{
    for (unsigned i = 0; i < AIC_DESTS; i++) {
        AICBank *b = &s->bank[i];
        bool timer = b->timer_pending && (b->control & 1);
        qemu_set_irq(b->irq, aic_pending_irq(s, i) >= 0 || (b->ipi_pending & ~b->ipi_mask) ||
                                 (timer && !(b->other_mask & 2)) || (b->nmi_pending && !(b->other_mask & 4)));
        qemu_set_irq(b->fiq, (timer && !(b->control & 2)) || (b->nmi_pending && !(b->control & 8)));
    }
}

static void aic_set_irq(void *opaque, int irq, int level)
{
    S5L8940AICState *s = opaque;
    uint32_t bit = 1u << (irq % 32);
    if (level) {
        s->levels[irq / 32] |= bit;
    } else {
        s->levels[irq / 32] &= ~bit;
    }
    aic_update(s);
}

static void aic_timer_expired(void *opaque)
{
    AICBank *b = opaque;
    b->timer_pending = true;
    if ((b->control & 1) && b->reload) {
        b->deadline_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + muldiv64(b->reload, NANOSECONDS_PER_SECOND, AIC_HZ);
        timer_mod(b->timer, b->deadline_ns);
    }
    aic_update(b->owner);
}

static uint32_t aic_timer_remaining(AICBank *b)
{
    int64_t left = b->deadline_ns - qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t ticks = left >= 0 ? muldiv64(left, AIC_HZ, NANOSECONDS_PER_SECOND)
                              : -(int64_t)muldiv64(-left, AIC_HZ, NANOSECONDS_PER_SECOND);
    return (uint32_t)(int32_t)MAX(INT32_MIN, MIN(ticks, INT32_MAX));
}

static AICBank *aic_bank(S5L8940AICState *s, hwaddr *offset)
{
    unsigned id = aic_current_id();
    if (*offset >= 0x5000 && *offset < 0x5000 + AIC_DESTS * 0x80) {
        id = (*offset - 0x5000) / 0x80;
        *offset = 0x2000 + (*offset & 0x7f);
    }
    return &s->bank[id];
}

static uint64_t aic_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8940AICState *s = opaque;
    AICBank *b = aic_bank(s, &offset);
    switch (offset) {
    case 0:
        return 1;
    case 4:
        return ((AIC_DESTS - 1) << 16) | AIC_EXT_IRQS;
    case 0x10:
        return s->config;
    case 0x20:
        return (uint32_t)aic_ticks(s);
    case 0x28:
        return aic_ticks(s) >> 32;
    case 0x2000:
        return b->id;
    case 0x2004: {
        int irq = aic_pending_irq(s, b->id);
        uint32_t result = 0;
        if (irq >= 0) {
            s->mask[irq / 32] |= 1u << (irq % 32);
            result = 0x10000 | irq;
        } else if (b->timer_pending && (b->control & 1) && !(b->other_mask & 2)) {
            b->other_mask |= 2;
            result = 0x70001;
        } else if (b->nmi_pending && !(b->other_mask & 4)) {
            b->other_mask |= 4;
            b->nmi_pending = false;
            result = 0x70004;
        } else if (b->ipi_pending & ~b->ipi_mask) {
            result = 0x40000 | ((b->ipi_pending & ~b->ipi_mask & 1) ? 1 : 0);
        }
        aic_update(s);
        return result;
    }
    case 0x2010:
        return b->control;
    case 0x2014:
        return aic_timer_remaining(b);
    case 0x2018:
        return b->timer_pending;
    case 0x2020:
        return b->other_mask;
    case 0x2028:
        return b->ipi_mask;
    }
    if (offset >= 0x3000 && offset < 0x3000 + AIC_EXT_IRQS * 4) {
        return s->target[(offset - 0x3000) / 4];
    }
    if (offset >= 0x4100 && offset < 0x4100 + AIC_WORDS * 4) {
        return s->mask[(offset - 0x4100) / 4];
    }
    if (offset >= 0x4180 && offset < 0x4180 + AIC_WORDS * 4) {
        return s->mask[(offset - 0x4180) / 4];
    }
    if (offset >= 0x4200 && offset < 0x4200 + AIC_WORDS * 4) {
        return s->levels[(offset - 0x4200) / 4] | s->software[(offset - 0x4200) / 4];
    }
    qemu_log_mask(LOG_UNIMP, "s5l8940.aic read +0x%" HWADDR_PRIx "\n", offset);
    return 0;
}

static void aic_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    S5L8940AICState *s = opaque;
    AICBank *b = aic_bank(s, &offset);
    uint32_t v = value;
    switch (offset) {
    case 0x10:
        s->config = v;
        break;
    case 0x2008:
        b->ipi_pending |= v;
        break;
    case 0x200c:
        b->ipi_pending &= ~v;
        break;
    case 0x2010:
        b->control = v;
        if (!(v & 1)) {
            timer_del(b->timer);
        } else {
            timer_mod(b->timer, MAX(b->deadline_ns, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)));
        }
        break;
    case 0x2014:
        b->reload = v;
        b->timer_pending = false;
        b->deadline_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + muldiv64(v, NANOSECONDS_PER_SECOND, AIC_HZ);
        if (b->control & 1) {
            timer_mod(b->timer, b->deadline_ns);
        }
        break;
    case 0x2018:
        if (v & 1) {
            b->timer_pending = false;
        }
        break;
    case 0x201c:
        if (v & 1)
            b->nmi_pending = true;
        b->other_mask |= v & ~1u;
        break;
    case 0x2020:
        b->other_mask &= ~v;
        break;
    case 0x4118:
        b->other_mask |= v;
        break;
    case 0x4198:
        b->other_mask &= ~v;
        break;
    case 0x2024:
        b->ipi_mask |= v;
        break;
    case 0x2028:
        b->ipi_mask &= ~v;
        break;
    default:
        if (offset >= 0x3000 && offset < 0x3000 + AIC_EXT_IRQS * 4) {
            s->target[(offset - 0x3000) / 4] = v;
        } else if (offset >= 0x4000 && offset < 0x4000 + AIC_WORDS * 4) {
            s->software[(offset - 0x4000) / 4] |= v;
        } else if (offset >= 0x4080 && offset < 0x4080 + AIC_WORDS * 4) {
            s->software[(offset - 0x4080) / 4] &= ~v;
        } else if (offset >= 0x4100 && offset < 0x4100 + AIC_WORDS * 4) {
            s->mask[(offset - 0x4100) / 4] |= v;
        } else if (offset >= 0x4180 && offset < 0x4180 + AIC_WORDS * 4) {
            s->mask[(offset - 0x4180) / 4] &= ~v;
        } else {
            qemu_log_mask(LOG_UNIMP, "s5l8940.aic write +0x%" HWADDR_PRIx " = 0x%x\n", offset, v);
        }
    }
    aic_update(s);
}

static const MemoryRegionOps aic_ops = {
    .read = aic_read,
    .write = aic_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {.min_access_size = 4, .max_access_size = 4},
    .impl = {.min_access_size = 4, .max_access_size = 4},
};

static void aic_reset(DeviceState *dev)
{
    S5L8940AICState *s = S5L8940_AIC(dev);
    s->epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->config = 0;
    for (unsigned i = 0; i < AIC_DESTS; i++) {
        AICBank *b = &s->bank[i];
        timer_del(b->timer);
        b->deadline_ns = s->epoch_ns;
        b->timer_pending = false;
        b->control = b->reload = b->ipi_pending = 0;
        b->ipi_mask = UINT32_MAX;
        b->other_mask = UINT32_MAX;
        b->nmi_pending = false;
    }
    memset(s->levels, 0, sizeof(s->levels));
    memset(s->software, 0, sizeof(s->software));
    memset(s->target, 0, sizeof(s->target));
    memset(s->mask, 0xff, sizeof(s->mask));
    aic_update(s);
}

static void aic_init(Object *obj)
{
    S5L8940AICState *s = S5L8940_AIC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    memory_region_init_io(&s->mmio, obj, &aic_ops, s, TYPE_S5L8940_AIC, 0x10000);
    sysbus_init_mmio(sbd, &s->mmio);
    for (unsigned i = 0; i < AIC_DESTS; i++) {
        AICBank *b = &s->bank[i];
        b->owner = s;
        b->id = i;
        sysbus_init_irq(sbd, &b->irq);
        sysbus_init_irq(sbd, &b->fiq);
        b->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, aic_timer_expired, b);
    }
    qdev_init_gpio_in(DEVICE(obj), aic_set_irq, AIC_EXT_IRQS);
}

static void aic_finalize(Object *obj)
{
    S5L8940AICState *s = S5L8940_AIC(obj);
    for (unsigned i = 0; i < AIC_DESTS; i++) {
        timer_free(s->bank[i].timer);
    }
}
static int aic_post_load(void *opaque, int version)
{
    aic_update(opaque);
    return 0;
}
static const VMStateDescription vmstate_aic_bank = {
    .name = "s5l8940.aic.bank",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]){VMSTATE_INT64(deadline_ns, AICBank), VMSTATE_UINT32(control, AICBank),
                                     VMSTATE_UINT32(reload, AICBank), VMSTATE_UINT32(ipi_pending, AICBank),
                                     VMSTATE_UINT32(ipi_mask, AICBank), VMSTATE_UINT32(other_mask, AICBank),
                                     VMSTATE_BOOL(nmi_pending, AICBank), VMSTATE_BOOL(timer_pending, AICBank),
                                     VMSTATE_TIMER_PTR(timer, AICBank), VMSTATE_END_OF_LIST()}};
static const VMStateDescription vmstate_aic = {
    .name = TYPE_S5L8940_AIC,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = aic_post_load,
    .fields = (const VMStateField[]){
        VMSTATE_INT64(epoch_ns, S5L8940AICState), VMSTATE_UINT32(config, S5L8940AICState),
        VMSTATE_UINT32_ARRAY(levels, S5L8940AICState, AIC_WORDS),
        VMSTATE_UINT32_ARRAY(software, S5L8940AICState, AIC_WORDS),
        VMSTATE_UINT32_ARRAY(mask, S5L8940AICState, AIC_WORDS),
        VMSTATE_UINT32_ARRAY(target, S5L8940AICState, AIC_EXT_IRQS),
        VMSTATE_STRUCT_ARRAY(bank, S5L8940AICState, AIC_DESTS, 1, vmstate_aic_bank, AICBank), VMSTATE_END_OF_LIST()}};
static void aic_class_init(ObjectClass *oc, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(oc), aic_reset);
    DEVICE_CLASS(oc)->vmsd = &vmstate_aic;
}
static const TypeInfo aic_type = {
    .name = TYPE_S5L8940_AIC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8940AICState),
    .instance_init = aic_init,
    .instance_finalize = aic_finalize,
    .class_init = aic_class_init,
};
static void aic_register_types(void)
{
    type_register_static(&aic_type);
}
type_init(aic_register_types)
