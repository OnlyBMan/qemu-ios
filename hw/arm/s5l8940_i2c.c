/* S5L8940 command-FIFO I2C controller. 10B329 AppleS5L8940XI2CController
 * queues START/address (bit 8), data bytes, and READ (bit 10)/STOP (bit 9)
 * tokens at +0. RX bytes come from +4 (bit 8 means empty). Completion,
 * NACK and RX-ready are W1C at +0x14, enabled as IRQs at +0x18.
 * Contracts: 807a5e58, 807a6584 and 807a661c in the N94 kernel.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/i2c/i2c.h"
#include "qom/object.h"
#include "migration/vmstate.h"

#define TYPE_S5L8940_I2C "s5l8940.i2c"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8940I2CState, S5L8940_I2C)
#define I2C_DONE (1u << 27)
#define I2C_IDLE (1u << 24)
#define I2C_BUSY (1u << 28)
#define I2C_NACK (1u << 21)
#define I2C_RX_READY (1u << 20)

struct S5L8940I2CState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    I2CBus *bus;
    qemu_irq irq;
    uint32_t status, mask, control, watermark;
    uint8_t rx[256];
    uint32_t rx_length, rx_position, tx_count;
    bool nack;
};

static void i2c_update(S5L8940I2CState *s)
{
    qemu_set_irq(s->irq, (s->status & s->mask & (I2C_DONE | I2C_NACK | I2C_RX_READY | 0x800040)) != 0);
}

static void i2c_finish(S5L8940I2CState *s)
{
    i2c_end_transfer(s->bus);
    s->status &= ~I2C_BUSY;
    s->status |= I2C_DONE | I2C_IDLE;
    if (s->nack) {
        s->status |= I2C_NACK;
    }
    i2c_update(s);
}

static void i2c_token(S5L8940I2CState *s, uint32_t value)
{
    s->tx_count++;
    if (value & 0x100) {
        s->status &= ~(I2C_DONE | I2C_IDLE | I2C_RX_READY);
        s->status |= I2C_BUSY;
        s->nack =
            (value & 1) ? i2c_start_recv(s->bus, (value >> 1) & 0x7f) : i2c_start_send(s->bus, (value >> 1) & 0x7f);
    } else if (value & 0x400) {
        unsigned length = value & 0xff;
        s->rx_length = s->rx_position = 0;
        for (unsigned i = 0; i < length && !s->nack; i++) {
            s->rx[s->rx_length++] = i2c_recv(s->bus);
        }
        s->status |= I2C_RX_READY;
        if (value & 0x200) {
            i2c_finish(s);
        }
    } else {
        if (!s->nack) {
            s->nack = i2c_send(s->bus, value & 0xff);
        }
        if (value & 0x200) {
            i2c_finish(s);
        }
    }
    i2c_update(s);
}

static uint64_t i2c_read(void *opaque, hwaddr off, unsigned size)
{
    S5L8940I2CState *s = opaque;
    switch (off) {
    case 4:
        return s->rx_position < s->rx_length ? s->rx[s->rx_position++] : 0x100;
    case 8:
        return s->tx_count & 0xff;
    case 0xc:
        return 0;
    case 0x14:
        return s->status;
    case 0x18:
        return s->mask;
    case 0x1c:
        return s->control;
    case 0x2c:
        return s->watermark;
    default:
        qemu_log_mask(LOG_UNIMP, "s5l8940.i2c read +0x%" HWADDR_PRIx "\n", off);
        return 0;
    }
}

static void i2c_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    S5L8940I2CState *s = opaque;
    switch (off) {
    case 0:
        i2c_token(s, value);
        break;
    case 0x14:
        s->status &= ~value;
        break;
    case 0x18:
        s->mask = value;
        break;
    case 0x1c:
        s->control = value;
        if (value & 0x200) {
            i2c_end_transfer(s->bus);
            s->status = s->rx_length = s->rx_position = s->tx_count = 0;
        }
        break;
    case 0x2c:
        s->watermark = value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "s5l8940.i2c write +0x%" HWADDR_PRIx " = 0x%x\n", off, (unsigned)value);
    }
    i2c_update(s);
}

static const MemoryRegionOps i2c_ops = {
    .read = i2c_read,
    .write = i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {.min_access_size = 4, .max_access_size = 4},
    .impl = {.min_access_size = 4, .max_access_size = 4},
};
static void i2c_reset(DeviceState *dev)
{
    S5L8940I2CState *s = S5L8940_I2C(dev);
    i2c_end_transfer(s->bus);
    s->status = s->mask = s->control = s->watermark = 0;
    s->rx_length = s->rx_position = s->tx_count = 0;
    s->nack = false;
    i2c_update(s);
}
static void i2c_init(Object *obj)
{
    S5L8940I2CState *s = S5L8940_I2C(obj);
    memory_region_init_io(&s->mmio, obj, &i2c_ops, s, TYPE_S5L8940_I2C, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->bus = i2c_init_bus(DEVICE(obj), "i2c");
}
static int i2c_post_load(void *opaque, int version)
{
    S5L8940I2CState *s = opaque;
    if (s->rx_length > sizeof(s->rx) || s->rx_position > s->rx_length)
        return -EINVAL;
    i2c_update(s);
    return 0;
}
static const VMStateDescription vmstate_i2c = {
    .name = TYPE_S5L8940_I2C,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = i2c_post_load,
    .fields = (const VMStateField[]){
        VMSTATE_UINT32(status, S5L8940I2CState), VMSTATE_UINT32(mask, S5L8940I2CState),
        VMSTATE_UINT32(control, S5L8940I2CState), VMSTATE_UINT32(watermark, S5L8940I2CState),
        VMSTATE_UINT8_ARRAY(rx, S5L8940I2CState, 256), VMSTATE_UINT32(rx_length, S5L8940I2CState),
        VMSTATE_UINT32(rx_position, S5L8940I2CState), VMSTATE_UINT32(tx_count, S5L8940I2CState),
        VMSTATE_BOOL(nack, S5L8940I2CState), VMSTATE_END_OF_LIST()}};
static void i2c_class_init(ObjectClass *oc, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(oc), i2c_reset);
    DEVICE_CLASS(oc)->vmsd = &vmstate_i2c;
}
static const TypeInfo i2c_type = {
    .name = TYPE_S5L8940_I2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8940I2CState),
    .instance_init = i2c_init,
    .class_init = i2c_class_init,
};
static void i2c_register_types(void)
{
    type_register_static(&i2c_type);
}
type_init(i2c_register_types)
