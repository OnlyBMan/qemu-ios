/*
 * iPhone 4S (N94AP, S5L8940/A5). The stock 10B329 DeviceTree places
 * arm-io at 0x30000000 and DRAM at 0x80000000. Boot through a firmwarekit
 * K48KBOOT bundle; the Cortex-A9 enters XNU with MMU off and r0=boot_args.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "chardev/char.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/boards.h"
#include "hw/arm/machines-qom.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/arm/exynos4210.h"
#include "hw/arm/ipod_touch_mipi_dsi.h"
#include "hw/arm/ipod_touch_spi.h"
#include "hw/arm/ipod_touch_buttons.h"
#include "hw/arm/ipod_touch_multitouch.h"
#include "ui/input.h"
#include "ui/console.h"
#include "hw/i2c/i2c.h"
#include "hw/usb/hcd-ehci.h"
#include "hw/usb/hcd-ohci.h"
#include "hw/arm/s5l8930.h"
#include "hw/misc/unimp.h"
#include "system/address-spaces.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "system/system.h"
#include "target/arm/cpu.h"
#include "target/arm/arm-powerctl.h"
#include "target/arm/cpregs.h"
#include "hw/arm/guest-services/general.h"
#include "hw/arm/guest-services/gles.h"
#include "hw/arm/guest-pasteboard.h"
#include "hw/arm/guest-package.h"
#include "hw/arm/ipod-agent.h"
#include "hw/arm/ipod_touch_cs42l58.h"
#include "qemu/guest-random.h"
#include "qom/object.h"
#include "migration/vmstate.h"
#include "trace.h"

#define TYPE_IPHONE4S_MACHINE MACHINE_TYPE_NAME("iPhone-4S")
OBJECT_DECLARE_SIMPLE_TYPE(IPhone4SMachineState, IPHONE4S_MACHINE)
#define N94_DRAM_BASE 0x80000000ULL
#define N94_DRAM_SIZE 0x20000000ULL

typedef struct N94AE2IRQBank {
    IPhone4SMachineState *owner;
    unsigned id;
} N94AE2IRQBank;

struct IPhone4SMachineState {
    MachineState parent_obj;
    ARMCPU *cpu, *iop_cpu, *ae2_cpu;
    MemoryRegion ae2_mem, ae2_dram, ae2_dram_high, ae2_periph, ae2_fw;
    MemoryRegion ae2_control, ae2_irq_ap, ae2_irq_local, ae2_acs, ae2_wgt, ae2_timer_mmio, ae2_sram;
    uint32_t ae2_reg[0x1000 / 4], ae2_acs_reg[0x1000 / 4], ae2_wgt_reg[0x1000 / 4];
    N94AE2IRQBank ae2_irq_bank[2];
    uint32_t ae2_pending[2], ae2_mask[2], ae2_fiq_mask;
    uint32_t ae2_timer_control, ae2_timer_reload;
    QEMUTimer *ae2_timer;
    qemu_irq ae2_ap_irq[2];
    bool ae2_fw_mapped;
    MemoryRegion dram, low, sac;
    uint32_t sac_reg[0x1000 / 4];
    MemoryRegion iop_mem, iop_dram, iop_dram_high, iop_periph, iop_fw, iop_regs;
    uint32_t iop_reg[0x1000 / 4];
    bool iop_fw_mapped;
    Chardev *gauge;
    IPodTouchMultitouchState *mt;
    DeviceState *display, *gpio, *pmu, *pmgr, *aic;
    QEMUTimer *ap_off_timer;
    bool resume_boot;
    GuestPasteboard pb;
    GuestPackage pkg;
    IPodAgent *agent;
    char *kboot;
    char *nand;
    char *nand_overlay;
};

static void n94_qemu_call(CPUARMState *env, const ARMCPRegInfo *ri, uint64_t value)
{
    CPUState *cs = env_cpu(env);
    IPhone4SMachineState *s = IPHONE4S_MACHINE(qdev_get_machine());
    qemu_call_t q;
    int32_t err = 0;

    if (qemu_call_rw(cs, value, &q, false)) {
        return;
    }
    switch (q.call_number) {
    case QC_GLES:
        q.retval = qc_handle_gles(cs, &q.args.gles);
        break;
    case QC_GLES_PING:
        q.retval = QC_GLES_PING_MAGIC;
        break;
    case QC_AG_HELLO:
    case QC_AG_POLL:
    case QC_AG_READ:
    case QC_AG_WRITE:
    case QC_AG_DONE:
    case QC_AG_HOSTTIME:
    case QC_UI_POLL:
    case QC_UI_READ:
    case QC_UI_WRITE:
    case QC_UI_DONE:
    case QC_AG_UI_ROUTE: {
        uint64_t candidate = 0;
        if (q.call_number == QC_AG_HELLO || q.call_number == QC_AG_UI_ROUTE) {
            qemu_guest_getrandom_nofail(&candidate, sizeof(candidate));
        }
        q.retval =
            ipod_agent_call(s->agent, q.call_number, q.args.ag.token, q.args.ag.buffer_guest_ptr, q.args.ag.offset,
                            q.args.ag.length, qemu_clock_get_ms(QEMU_CLOCK_REALTIME), candidate, guest_copy, cs);
        err = q.retval < 0 ? EINVAL : 0;
        break;
    }
    default:
        if (!guest_pb_call(&s->pb, cs, &q, &err) && !guest_pkg_call(&s->pkg, cs, &q, &err)) {
            return;
        }
    }
    q.error = err;
    qemu_call_rw(cs, value, &q, true);
}

static const ARMCPRegInfo n94_cp_reginfo[] = {
    {.name = "QEMU_CALL",
     .cp = 15,
     .opc1 = 3,
     .crn = 15,
     .crm = 15,
     .opc2 = 0,
     .access = PL0_RW,
     .state = ARM_CP_STATE_AA32,
     .type = ARM_CP_IO | ARM_CP_NO_RAW | ARM_CP_RAISES_EXC, /* gles_guest_rw */
     .readfn = qemu_call_status,
     .writefn = n94_qemu_call},
};

static uint64_t n94_iop_read(void *opaque, hwaddr off, unsigned size)
{
    IPhone4SMachineState *s = opaque;
    return s->iop_reg[off / 4];
}

static bool n94_map_iop(IPhone4SMachineState *s)
{
    uint32_t base = s->iop_reg[1];
    if (base < N94_DRAM_BASE || (uint64_t)base + 0x40000 > N94_DRAM_BASE + N94_DRAM_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "n94 IOP firmware outside DRAM: 0x%x\n", base);
        return false;
    }
    if (s->iop_fw_mapped) {
        memory_region_del_subregion(&s->iop_mem, &s->iop_fw);
        object_unparent(OBJECT(&s->iop_fw));
    }
    memory_region_init_alias(&s->iop_fw, NULL, "n94.iop-firmware", &s->dram, base - N94_DRAM_BASE, 0x40000);
    memory_region_add_subregion(&s->iop_mem, 0, &s->iop_fw);
    s->iop_fw_mapped = true;
    return true;
}

static void n94_iop_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    IPhone4SMachineState *s = opaque;
    s->iop_reg[off / 4] = value;
    if (off != 0) {
        return;
    }
    cpu_reset(CPU(s->iop_cpu));
    if (!(value & 1)) {
        return;
    }
    if (!n94_map_iop(s))
        return;
    s->iop_cpu->power_state = PSCI_ON;
    s->iop_cpu->env.halt_reason = NOT_HALTED;
    CPU(s->iop_cpu)->halted = false;
    cpu_set_pc(CPU(s->iop_cpu), 0);
    qemu_cpu_kick(CPU(s->iop_cpu));
    qemu_log_mask(LOG_UNIMP, "n94 IOP starting firmware at 0x%x\n", s->iop_reg[1]);
}

/* AE2 is an independent embedded ARM processor. AP doorbells live at
 * 341e1000; the firmware sees its own mask/pending/doorbell bank at 341a1000.
 * The firmware image is supplied by XNU, including its writable data and BSS. */
static void n94_ae2_update(IPhone4SMachineState *s)
{
    uint32_t pending = s->ae2_pending[1];
    qemu_set_irq(qdev_get_gpio_in(DEVICE(s->ae2_cpu), ARM_CPU_IRQ), pending & s->ae2_mask[1]);
    qemu_set_irq(qdev_get_gpio_in(DEVICE(s->ae2_cpu), ARM_CPU_FIQ), pending & s->ae2_fiq_mask);
    for (int n = 0; n < 2; n++) {
        qemu_set_irq(s->ae2_ap_irq[n], s->ae2_pending[0] & s->ae2_mask[0] & (1u << (4 + n)));
    }
}

static bool n94_map_ae2(IPhone4SMachineState *s)
{
    uint32_t base = s->ae2_reg[2];
    if (base < N94_DRAM_BASE || (uint64_t)base + 0x80000 > N94_DRAM_BASE + N94_DRAM_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "n94 AE2 firmware outside DRAM: 0x%x\n", base);
        return false;
    }
    if (s->ae2_fw_mapped) {
        memory_region_del_subregion(&s->ae2_mem, &s->ae2_fw);
        object_unparent(OBJECT(&s->ae2_fw));
    }
    memory_region_init_alias(&s->ae2_fw, NULL, "n94.ae2-firmware", &s->dram, base - N94_DRAM_BASE, 0x80000);
    memory_region_add_subregion(&s->ae2_mem, 0, &s->ae2_fw);
    s->ae2_fw_mapped = true;
    return true;
}

static uint64_t n94_ae2_control_read(void *opaque, hwaddr off, unsigned size)
{
    return ((IPhone4SMachineState *)opaque)->ae2_reg[off / 4];
}

static void n94_ae2_control_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    IPhone4SMachineState *s = opaque;
    uint32_t old = s->ae2_reg[off / 4];
    s->ae2_reg[off / 4] = value;
    trace_iphone4s_ae2_control(off, value);
    if (off != 4 || !((old ^ value) & 1)) {
        return;
    }
    cpu_reset(CPU(s->ae2_cpu));
    if (!(value & 1) || !n94_map_ae2(s)) {
        return;
    }
    s->ae2_cpu->power_state = PSCI_ON;
    s->ae2_cpu->env.halt_reason = NOT_HALTED;
    CPU(s->ae2_cpu)->halted = false;
    cpu_set_pc(CPU(s->ae2_cpu), 0);
    qemu_cpu_kick(CPU(s->ae2_cpu));
    n94_ae2_update(s);
}

static uint64_t n94_ae2_irq_read(void *opaque, hwaddr off, unsigned size)
{
    N94AE2IRQBank *b = opaque;
    IPhone4SMachineState *s = b->owner;
    unsigned bank = b->id;
    switch (off) {
    case 0:
        return s->ae2_mask[bank];
    case 4:
        return s->ae2_pending[bank];
    case 0x10:
        return bank ? s->ae2_fiq_mask : 0;
    case 0x14:
        return s->ae2_pending[bank];
    }
    return 0;
}

static void n94_ae2_irq_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    N94AE2IRQBank *b = opaque;
    IPhone4SMachineState *s = b->owner;
    unsigned bank = b->id;
    trace_iphone4s_ae2_irq(bank, off, value);
    switch (off) {
    case 8:
        s->ae2_mask[bank] |= value;
        break;
    case 0xc:
        s->ae2_mask[bank] &= ~value;
        break;
    case 0x18:
        if (bank)
            s->ae2_fiq_mask |= value;
        break;
    case 0x1c:
        if (bank)
            s->ae2_fiq_mask &= ~value;
        break;
    case 0x20:
        s->ae2_pending[bank ^ 1] |= value;
        break;
    case 0x24:
        s->ae2_pending[bank] &= ~value;
        break;
    }
    n94_ae2_update(s);
}

static uint64_t n94_ae2_acs_read(void *opaque, hwaddr off, unsigned size)
{
    return ((IPhone4SMachineState *)opaque)->ae2_acs_reg[off / 4];
}

static void n94_ae2_acs_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    ((IPhone4SMachineState *)opaque)->ae2_acs_reg[off / 4] = value & ~0x10000u;
}

static uint64_t n94_ae2_wgt_read(void *opaque, hwaddr off, unsigned size)
{
    return ((IPhone4SMachineState *)opaque)->ae2_wgt_reg[off / 4];
}

static void n94_ae2_wgt_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    ((IPhone4SMachineState *)opaque)->ae2_wgt_reg[off / 4] = value;
}

static void n94_ae2_timer_expired(void *opaque)
{
    IPhone4SMachineState *s = opaque;
    s->ae2_pending[1] |= 1;
    n94_ae2_update(s);
}

static uint64_t n94_ae2_timer_read(void *opaque, hwaddr off, unsigned size)
{
    IPhone4SMachineState *s = opaque;
    return off == 0 ? s->ae2_timer_control : s->ae2_timer_reload;
}

static void n94_ae2_timer_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    IPhone4SMachineState *s = opaque;
    if (off == 4) {
        s->ae2_timer_reload = value;
    } else if (off == 0) {
        s->ae2_timer_control = value;
        timer_del(s->ae2_timer);
        s->ae2_pending[1] &= ~1u;
        if (value & 1) {
            timer_mod(s->ae2_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                        muldiv64(s->ae2_timer_reload, NANOSECONDS_PER_SECOND, 24000000));
        }
        n94_ae2_update(s);
    }
}

#define N94_AE2_OPS(name)                                                                                              \
    static const MemoryRegionOps n94_ae2_##name##_ops = {                                                              \
        .read = n94_ae2_##name##_read,                                                                                 \
        .write = n94_ae2_##name##_write,                                                                               \
        .endianness = DEVICE_LITTLE_ENDIAN,                                                                            \
        .valid = {.min_access_size = 4, .max_access_size = 4},                                                         \
        .impl = {.min_access_size = 4, .max_access_size = 4},                                                          \
    }
N94_AE2_OPS(control);
N94_AE2_OPS(irq);
N94_AE2_OPS(acs);
N94_AE2_OPS(wgt);
N94_AE2_OPS(timer);

/* DSI slow-adaptive clocking uses the PMGR SAC bank. Bit 8 reports
 * the enabled clock locked; the stock panel driver polls it on display wake. */
static uint64_t n94_sac_read(void *opaque, hwaddr off, unsigned size)
{
    IPhone4SMachineState *s = opaque;
    uint32_t value = s->sac_reg[off / 4];
    return off == 0 && (value & 1) ? value | 0x100 : value;
}

static void n94_sac_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    IPhone4SMachineState *s = opaque;
    s->sac_reg[off / 4] = off == 0 ? value & ~0x100u : value;
}

static const MemoryRegionOps n94_sac_ops = {
    .read = n94_sac_read,
    .write = n94_sac_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {.min_access_size = 4, .max_access_size = 4},
    .impl = {.min_access_size = 4, .max_access_size = 4},
};

static int n94_post_load(void *opaque, int version)
{
    IPhone4SMachineState *s = opaque;
    if ((s->iop_reg[0] & 1) && !n94_map_iop(s))
        return -EINVAL;
    if ((s->ae2_reg[1] & 1) && !n94_map_ae2(s))
        return -EINVAL;
    n94_ae2_update(s);
    return 0;
}
static const VMStateDescription vmstate_n94 = {
    .name = "iphone4s.iop-control",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = n94_post_load,
    .fields = (const VMStateField[]){
        VMSTATE_UINT32_ARRAY(iop_reg, IPhone4SMachineState, 0x1000 / 4),
        VMSTATE_BOOL(resume_boot, IPhone4SMachineState), VMSTATE_TIMER_PTR(ap_off_timer, IPhone4SMachineState),
        VMSTATE_UINT32_ARRAY(sac_reg, IPhone4SMachineState, 0x1000 / 4),
        VMSTATE_UINT32_ARRAY(ae2_reg, IPhone4SMachineState, 0x1000 / 4),
        VMSTATE_UINT32_ARRAY(ae2_acs_reg, IPhone4SMachineState, 0x1000 / 4),
        VMSTATE_UINT32_ARRAY(ae2_wgt_reg, IPhone4SMachineState, 0x1000 / 4),
        VMSTATE_UINT32_ARRAY(ae2_pending, IPhone4SMachineState, 2),
        VMSTATE_UINT32_ARRAY(ae2_mask, IPhone4SMachineState, 2), VMSTATE_UINT32(ae2_fiq_mask, IPhone4SMachineState),
        VMSTATE_UINT32(ae2_timer_control, IPhone4SMachineState), VMSTATE_UINT32(ae2_timer_reload, IPhone4SMachineState),
        VMSTATE_TIMER_PTR(ae2_timer, IPhone4SMachineState), VMSTATE_END_OF_LIST()}};

static const MemoryRegionOps n94_iop_ops = {
    .read = n94_iop_read,
    .write = n94_iop_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {.min_access_size = 4, .max_access_size = 4},
    .impl = {.min_access_size = 4, .max_access_size = 4},
};

static bool n94_resume(IPhone4SMachineState *s)
{
    uint32_t marker[2];
    address_space_read(&address_space_memory, N94_DRAM_BASE + 0x80, MEMTXATTRS_UNSPECIFIED, marker, sizeof(marker));
    if (le32_to_cpu(marker[0]) != 0x4d4f5358 || le32_to_cpu(marker[1]) != 0x53555350 ||
        !s5l8940_d1881_take_suspend(s->pmu))
        return false;
    address_space_set(&address_space_memory, N94_DRAM_BASE + 0x80, 0, sizeof(marker), MEMTXATTRS_UNSPECIFIED);
    s->cpu->env.regs[0] = 0;
    cpu_set_pc(CPU(s->cpu), 0);
    return true;
}

static void n94_ap_cut(void *opaque)
{
    IPhone4SMachineState *s = opaque;
    arm_set_cpu_off(arm_cpu_mp_affinity(s->cpu));
}

static void n94_ap_power(void *opaque, int n, int on)
{
    IPhone4SMachineState *s = opaque;
    if (on) {
        timer_del(s->ap_off_timer);
        s->resume_boot = true;
        object_property_set_bool(OBJECT(s->pmgr), "keep-timebase", true, &error_abort);
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    } else {
        /* XNU writes its DRAM suspend marker after issuing the PMU command. */
        timer_mod(s->ap_off_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 20);
    }
}

static void iphone4s_cpu_reset(void *opaque)
{
    IPhone4SMachineState *s = opaque;
    g_autofree char *data = NULL;
    g_autoptr(GError) err = NULL;
    gsize size;
    const uint8_t *tail;
    uint32_t load, entry, args, length;

    bool resume = s->resume_boot;
    s->resume_boot = false;
    timer_del(s->ap_off_timer);
    cpu_reset(CPU(s->cpu));
    memset(s->iop_reg, 0, sizeof(s->iop_reg));
    memset(s->sac_reg, 0, sizeof(s->sac_reg));
    memset(s->ae2_reg, 0, sizeof(s->ae2_reg));
    memset(s->ae2_acs_reg, 0, sizeof(s->ae2_acs_reg));
    memset(s->ae2_wgt_reg, 0, sizeof(s->ae2_wgt_reg));
    memset(s->ae2_pending, 0, sizeof(s->ae2_pending));
    memset(s->ae2_mask, 0, sizeof(s->ae2_mask));
    s->ae2_fiq_mask = s->ae2_timer_control = s->ae2_timer_reload = 0;
    timer_del(s->ae2_timer);
    n94_ae2_update(s);
    if (resume && n94_resume(s))
        return;
    gles_host_reset();
    guest_pkg_reset(&s->pkg);
    ipod_agent_reset(s->agent);
    if (!g_file_get_contents(s->kboot, &data, &size, &err)) {
        error_report("iPhone-4S: cannot read kboot: %s", err->message);
        exit(1);
    }
    if (size < 24 || memcmp(data + size - 24, "K48KBOOT", 8)) {
        error_report("iPhone-4S: invalid K48KBOOT bundle");
        exit(1);
    }
    tail = (uint8_t *)data + size - 24;
    load = ldl_le_p(tail + 8);
    entry = ldl_le_p(tail + 12);
    args = ldl_le_p(tail + 16);
    length = ldl_le_p(tail + 20);
    if (length > size - 24 || load < N94_DRAM_BASE || (uint64_t)load + length > N94_DRAM_BASE + N94_DRAM_SIZE ||
        entry < load || entry >= (uint64_t)load + length || args < load ||
        (uint64_t)args + 312 > (uint64_t)load + length) {
        error_report("iPhone-4S: kboot addresses outside DRAM/image");
        exit(1);
    }
    address_space_write(&address_space_memory, load, MEMTXATTRS_UNSPECIFIED, data, length);
    s->cpu->env.regs[0] = args;
    cpu_set_pc(CPU(s->cpu), entry);
}

static void iphone4s_reset(MachineState *machine, ResetType type)
{
    qemu_devices_reset(type);
}

void iphone4s_press_button(IPodTouchButton button, bool down)
{
    Object *machine = object_dynamic_cast(OBJECT(qdev_get_machine()), TYPE_IPHONE4S_MACHINE);
    static const unsigned pins[] = {0, 1, 3, 2};
    if (machine && (unsigned)button < ARRAY_SIZE(pins)) {
        IPhone4SMachineState *s = IPHONE4S_MACHINE(machine);
        if (s->gpio)
            qemu_set_irq(qdev_get_gpio_in(s->gpio, pins[button]), !down);
        if (s->pmu && (button == IPOD_TOUCH_BUTTON_HOME || button == IPOD_TOUCH_BUTTON_POWER))
            s5l8940_d1881_button(s->pmu, button == IPOD_TOUCH_BUTTON_POWER, down);
    }
}

static void n94_mouse_event(void *opaque, int x, int y, int z, int buttons)
{
    IPodTouchMultitouchState *mt = opaque;
    mt->touch_x = x / 32768.0f;
    mt->touch_y = 1.0f - y / 32768.0f;
    if (buttons && !mt->touch_down) {
        ipod_touch_multitouch_on_touch(mt);
    } else if (!buttons && mt->touch_down) {
        ipod_touch_multitouch_on_release(mt);
    } else if (buttons) {
        ipod_touch_multitouch_on_motion(mt);
    }
}

static void iphone4s_init(MachineState *machine)
{
    IPhone4SMachineState *s = IPHONE4S_MACHINE(machine);
    MemoryRegion *mem = get_system_memory();
    Object *cpu;
    DeviceState *pmgr, *aic, *gpio, *i2c, *pmu, *dev, *iop_store, *cdma;
    SysBusDevice *sbd;

    if (!s->kboot) {
        error_report("iPhone-4S: kboot= is required");
        exit(1);
    }
    cpu = object_new(machine->cpu_type);
    s->cpu = ARM_CPU(cpu);
    object_property_set_link(cpu, "memory", OBJECT(mem), &error_abort);
    object_property_set_bool(cpu, "has_el3", false, &error_abort);
    object_property_set_bool(cpu, "realized", true, &error_fatal);
    define_arm_cp_regs(s->cpu, n94_cp_reginfo);
    object_unref(cpu);

    memory_region_init_ram(&s->dram, NULL, "n94.dram", N94_DRAM_SIZE, &error_fatal);
    memory_region_add_subregion(mem, N94_DRAM_BASE, &s->dram);
    memory_region_init_alias(&s->low, OBJECT(s), "n94.reset-vectors", &s->dram, 0, 0x1000);
    memory_region_add_subregion(mem, 0, &s->low);
    create_unimplemented_device("s5l8940.periph", 0x30000000, 0x10000000);

    aic = sysbus_create_varargs("s5l8940.aic", 0x3f200000, qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_IRQ),
                                qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_FIQ), NULL);
    s->aic = aic;
    s->ap_off_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, n94_ap_cut, s);
    memory_region_init(&s->iop_mem, NULL, "n94.iop-memory", 1ULL << 32);
    memory_region_init_alias(&s->iop_dram, NULL, "n94.iop-dram", &s->dram, 0, N94_DRAM_SIZE);
    memory_region_add_subregion(&s->iop_mem, 0x40000000, &s->iop_dram);
    memory_region_init_alias(&s->iop_dram_high, NULL, "n94.iop-dram-high", &s->dram, 0, N94_DRAM_SIZE);
    memory_region_add_subregion(&s->iop_mem, N94_DRAM_BASE, &s->iop_dram_high);
    memory_region_init_alias(&s->iop_periph, NULL, "n94.iop-peripherals", mem, 0x30000000, 0x10000000);
    memory_region_add_subregion(&s->iop_mem, 0x30000000, &s->iop_periph);
    cpu = object_new(ARM_CPU_TYPE_NAME("cortex-a9"));
    s->iop_cpu = ARM_CPU(cpu);
    object_property_set_link(cpu, "memory", OBJECT(&s->iop_mem), &error_abort);
    object_property_set_bool(cpu, "has_el3", false, &error_abort);
    object_property_set_int(cpu, "mp-affinity", 2, &error_abort);
    object_property_set_bool(cpu, "start-powered-off", true, &error_abort);
    object_property_set_bool(cpu, "realized", true, &error_fatal);
    object_unref(cpu);
    sysbus_connect_irq(SYS_BUS_DEVICE(aic), 4, qdev_get_gpio_in(DEVICE(s->iop_cpu), ARM_CPU_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(aic), 5, qdev_get_gpio_in(DEVICE(s->iop_cpu), ARM_CPU_FIQ));
    memory_region_init_io(&s->iop_regs, NULL, &n94_iop_ops, s, "n94.iop-control", 0x1000);
    memory_region_add_subregion(mem, 0x3f300000, &s->iop_regs);

    memory_region_init(&s->ae2_mem, NULL, "n94.ae2-memory", 1ULL << 32);
    memory_region_init_alias(&s->ae2_dram, NULL, "n94.ae2-dram", &s->dram, 0, N94_DRAM_SIZE);
    memory_region_add_subregion(&s->ae2_mem, 0x40000000, &s->ae2_dram);
    memory_region_init_alias(&s->ae2_dram_high, NULL, "n94.ae2-dram-high", &s->dram, 0, N94_DRAM_SIZE);
    memory_region_add_subregion(&s->ae2_mem, N94_DRAM_BASE, &s->ae2_dram_high);
    memory_region_init_alias(&s->ae2_periph, NULL, "n94.ae2-peripherals", mem, 0x30000000, 0x10000000);
    memory_region_add_subregion(&s->ae2_mem, 0x30000000, &s->ae2_periph);
    cpu = object_new(ARM_CPU_TYPE_NAME("cortex-a9"));
    s->ae2_cpu = ARM_CPU(cpu);
    object_property_set_link(cpu, "memory", OBJECT(&s->ae2_mem), &error_abort);
    object_property_set_bool(cpu, "has_el3", false, &error_abort);
    object_property_set_int(cpu, "mp-affinity", 3, &error_abort);
    object_property_set_bool(cpu, "start-powered-off", true, &error_abort);
    object_property_set_bool(cpu, "realized", true, &error_fatal);
    object_unref(cpu);
    s->ae2_ap_irq[0] = qdev_get_gpio_in(aic, 0x61);
    s->ae2_ap_irq[1] = qdev_get_gpio_in(aic, 0x62);
    s->ae2_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, n94_ae2_timer_expired, s);
    memory_region_init_ram(&s->ae2_sram, NULL, "n94.ae2-sram", 0x30000, &error_fatal);
    memory_region_add_subregion(mem, 0x34000000, &s->ae2_sram);
    memory_region_init_io(&s->ae2_control, NULL, &n94_ae2_control_ops, s, "n94.ae2-control", 0x1000);
    memory_region_add_subregion(mem, 0x341e0000, &s->ae2_control);
    s->ae2_irq_bank[0] = (N94AE2IRQBank){s, 0};
    s->ae2_irq_bank[1] = (N94AE2IRQBank){s, 1};
    memory_region_init_io(&s->ae2_irq_ap, NULL, &n94_ae2_irq_ops, &s->ae2_irq_bank[0], "n94.ae2-irq-ap", 0x1000);
    memory_region_add_subregion(mem, 0x341e1000, &s->ae2_irq_ap);
    memory_region_init_io(&s->ae2_irq_local, NULL, &n94_ae2_irq_ops, &s->ae2_irq_bank[1], "n94.ae2-irq-local", 0x1000);
    memory_region_add_subregion(mem, 0x341a1000, &s->ae2_irq_local);
    memory_region_init_io(&s->ae2_acs, NULL, &n94_ae2_acs_ops, s, "n94.ae2-acs", 0x1000);
    memory_region_add_subregion(mem, 0x341a0000, &s->ae2_acs);
    memory_region_init_io(&s->ae2_wgt, NULL, &n94_ae2_wgt_ops, s, "n94.ae2-wgt", 0x1000);
    memory_region_add_subregion(mem, 0x341c1000, &s->ae2_wgt);
    memory_region_init_io(&s->ae2_timer_mmio, NULL, &n94_ae2_timer_ops, s, "n94.ae2-timer", 0x1000);
    memory_region_add_subregion(mem, 0x341a2000, &s->ae2_timer_mmio);

    dev = qdev_new(TYPE_S5L8930_DISPLAY);
    s->display = dev;
    qdev_prop_set_bit(dev, "a5", true);
    qdev_prop_set_uint64(dev, "fb-base", 0x9f700000);
    qdev_prop_set_uint16(dev, "width", 640);
    qdev_prop_set_uint16(dev, "height", 960);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, 0x3a100000);
    sysbus_mmio_map(sbd, 1, 0x39200000);
    sysbus_mmio_map(sbd, 2, 0x3a400000);
    sysbus_mmio_map(sbd, 3, 0x3a200000);
    sysbus_mmio_map(sbd, 4, 0x39400000);
    sysbus_mmio_map(sbd, 5, 0x39600000);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(aic, 0x2b));
    sysbus_connect_irq(sbd, 1, qdev_get_gpio_in(aic, 0x2a));
    sysbus_connect_irq(sbd, 2, qdev_get_gpio_in(aic, 0x2c));
    dev = qdev_new(TYPE_IPOD_TOUCH_MIPI_DSI);
    IPOD_TOUCH_MIPI_DSI(dev)->hs_clock_at_reset = true;
    qdev_prop_set_uint32(dev, "lanes", 4);
    qdev_prop_set_uint32(dev, "panel-id", 0x09d897a5);
    qdev_prop_set_uint32(dev, "panel-id-len", 4);
    memory_region_add_subregion(mem, 0x39500000, &IPOD_TOUCH_MIPI_DSI(dev)->iomem);
    sysbus_realize(SYS_BUS_DEVICE(dev), &error_fatal);

    for (int n = 0; n < 2; n++) {
        dev = sysbus_create_simple("ipodtouch.scaler", 0x38300000 + n * 0x100000, qdev_get_gpio_in(aic, 0x09 + n));
        ipod_scaler_set_iommu(dev, s5l8930_dart2_xlate, s->display, 2);
        ipod_scaler_set_version(dev, 0x20007);
    }

    iop_store = qdev_new(TYPE_S5L8930_IOP);
    qdev_prop_set_uint32(iop_store, "nand-id", 0x14e5050);
    qdev_prop_set_uint8(iop_store, "nand-ce-mask", 1);
    qdev_prop_set_uint32(iop_store, "nand-meta-bytes", 32);
    qdev_prop_set_uint32(iop_store, "nand-extra-pages", 4128 * 256);
    if (s->nand) {
        qdev_prop_set_string(iop_store, "nand", s->nand);
    }
    if (s->nand_overlay) {
        qdev_prop_set_string(iop_store, "nand-overlay", s->nand_overlay);
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(iop_store), &error_fatal);
    cdma = qdev_new(TYPE_S5L8930_CDMA);
    qdev_prop_set_uint64(cdma, "dram-base", N94_DRAM_BASE);
    qdev_prop_set_uint64(cdma, "dram-size", N94_DRAM_SIZE);
    qdev_prop_set_uint8(cdma, "channels", 0x2d);
    qdev_prop_set_uint8(cdma, "version", 3);
    qdev_prop_set_bit(cdma, "inline-aes", true);
    qdev_prop_set_uint64(cdma, "paced-base", 0x34190000);
    qdev_prop_set_uint32(cdma, "paced-ports", 4);
    sbd = SYS_BUS_DEVICE(cdma);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, 0x37000000);
    sysbus_mmio_map(sbd, 1, 0x37800000);
    for (int n = 1; n < 0x2d; n++) {
        sysbus_connect_irq(sbd, n, qdev_get_gpio_in(aic, 0x32 + n));
    }
    dev = qdev_new(TYPE_S5L8930_H2FMI);
    qdev_prop_set_uint64(dev, "base", 0x31200000);
    qdev_prop_set_bit(dev, "ppn", true);
    object_property_set_link(OBJECT(dev), "iop", OBJECT(iop_store), &error_fatal);
    object_property_set_link(OBJECT(dev), "cdma", OBJECT(cdma), &error_fatal);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    for (int n = 0; n < 2; n++) {
        sysbus_mmio_map(sbd, n, 0x31200000 + n * 0x100000);
        sysbus_connect_irq(sbd, n, qdev_get_gpio_in(aic, 0x21 + n));
    }

    dev = qdev_new(TYPE_EXYNOS4210_EHCI);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, 0x36400000);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(aic, 0x0c));
    for (int n = 0; n < 2; n++) {
        dev = qdev_new(TYPE_SYSBUS_OHCI);
        qdev_prop_set_uint32(dev, "num-ports", 1);
        sbd = SYS_BUS_DEVICE(dev);
        sysbus_realize_and_unref(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, 0x36500000 + n * 0x100000);
        sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(aic, 0x0d + n));
    }

    /* N94 has four Samsung I2S ports. Media services waits for TX_IDLE when
     * bringing up the CS42L63's master clock, before SpringBoard can launch. */
    for (int n = 0; n < 4; n++) {
        dev = qdev_new(TYPE_S5L8930_I2S);
        qdev_prop_set_uint8(dev, "port", n);
        sbd = SYS_BUS_DEVICE(dev);
        sysbus_realize_and_unref(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, 0x34190000 + n * 0x1000);
    }

    sysbus_create_simple("l2x0", 0x3e000000, NULL);
    pmgr = qdev_new(TYPE_S5L8930_PMGR);
    s->pmgr = pmgr;
    qdev_prop_set_uint32(pmgr, "gate-count", 0x80);
    qdev_prop_set_uint8(pmgr, "board-id", 0x08);
    sbd = SYS_BUS_DEVICE(pmgr);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, 0x3f100000);
    memory_region_init_io(&s->sac, NULL, &n94_sac_ops, s, "n94.dsi-sac", 0x1000);
    memory_region_add_subregion(mem, 0x3f110000, &s->sac);
    gpio = qdev_new(TYPE_S5L8930_GPIO);
    s->gpio = gpio;
    qdev_prop_set_uint32(gpio, "ports", 0x1c);
    qdev_prop_set_uint32(gpio, "int-groups", 8);

    sysbus_realize_and_unref(SYS_BUS_DEVICE(gpio), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(gpio), 0, 0x3fa00000);
    sysbus_connect_irq(SYS_BUS_DEVICE(gpio), 0, qdev_get_gpio_in(aic, 0x77));
    dev = qdev_new(TYPE_IPOD_TOUCH_SPI);
    qdev_prop_set_uint8(dev, "index", 1);
    qdev_prop_set_string(dev, "peripheral", "multitouch");
    qdev_prop_set_uint32(dev, "tx-fifo-depth", 0x10000);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0x32100000);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, qdev_get_gpio_in(aic, 0x1d));
    s->mt = IPOD_TOUCH_SPI(dev)->mt;
    s->mt->profile = &mt_profile_n81;
    qdev_connect_gpio_out_named(DEVICE(s->mt), "atn", 0, qdev_get_gpio_in(gpio, 0x15));
    qemu_add_mouse_event_handler(n94_mouse_event, s->mt, 1, "iPhone 4S Touchscreen");
    for (int n = 0; n < 3; n++) {
        i2c = sysbus_create_simple("s5l8940.i2c", 0x33200000 + n * 0x100000, qdev_get_gpio_in(aic, 0x12 + n));
        if (n == 0) {
            i2c_slave_create_simple(I2C_BUS(qdev_get_child_bus(i2c, "i2c")), TYPE_CS42L63, 0x4a);
            pmu = DEVICE(i2c_slave_create_simple(I2C_BUS(qdev_get_child_bus(i2c, "i2c")), "s5l8940.d1881", 0x74));
            s->pmu = pmu;
            qdev_connect_gpio_out_named(pmu, "ap-power", 0, qemu_allocate_irq(n94_ap_power, s, 0));
            qdev_connect_gpio_out(pmu, 0, qemu_irq_invert(qdev_get_gpio_in(gpio, S5L8930_GPIO_PIN(0x0d))));
        }
    }
    exynos4210_uart_create(0x32500000, 256, 0, serial_hd(0), qdev_get_gpio_in(aic, 0x15), true);
    /* Stock DT: bq27540 on UART5's HDQ line, with a 16-byte receive FIFO. */
    s->gauge = qemu_chardev_new(NULL, TYPE_CHARDEV_S5L8930_HDQ, NULL, NULL, &error_abort);
    s5l8930_hdq_set_capacity(s->gauge, 1432);
    s5l8930_hdq_set_battery(s->gauge, 95, false);
    exynos4210_uart_create(0x32a00000, 16, 5, s->gauge, qdev_get_gpio_in(aic, 0x1a), true);
    vmstate_register(NULL, 0, &vmstate_n94, s);
    qemu_register_reset(iphone4s_cpu_reset, s);
}

static char *iphone4s_get_kboot(Object *obj, Error **errp)
{
    return g_strdup(IPHONE4S_MACHINE(obj)->kboot);
}

static void iphone4s_set_kboot(Object *obj, const char *value, Error **errp)
{
    IPhone4SMachineState *s = IPHONE4S_MACHINE(obj);
    g_free(s->kboot);
    s->kboot = g_strdup(value);
}

static char *iphone4s_get_nand(Object *obj, Error **errp)
{
    return g_strdup(IPHONE4S_MACHINE(obj)->nand);
}

static void iphone4s_set_nand(Object *obj, const char *value, Error **errp)
{
    IPhone4SMachineState *s = IPHONE4S_MACHINE(obj);
    g_free(s->nand);
    s->nand = g_strdup(value);
}

static char *iphone4s_get_overlay(Object *obj, Error **errp)
{
    return g_strdup(IPHONE4S_MACHINE(obj)->nand_overlay);
}

static void iphone4s_set_overlay(Object *obj, const char *value, Error **errp)
{
    IPhone4SMachineState *s = IPHONE4S_MACHINE(obj);
    g_free(s->nand_overlay);
    s->nand_overlay = g_strdup(value);
}

static void iphone4s_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    object_class_property_add_str(oc, "nand", iphone4s_get_nand, iphone4s_set_nand);
    object_class_property_add_str(oc, "nand-overlay", iphone4s_get_overlay, iphone4s_set_overlay);
    mc->desc = "iPhone 4S (N94AP, S5L8940)";
    mc->init = iphone4s_init;
    mc->reset = iphone4s_reset;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a9");
    mc->default_ram_size = N94_DRAM_SIZE;
    mc->max_cpus = 3;
    mc->default_cpus = 3;
    mc->minimum_page_bits = 12;
    object_class_property_add_str(oc, "kboot", iphone4s_get_kboot, iphone4s_set_kboot);
}

static void n94_set_agent_request(Object *obj, const char *value, Error **errp)
{
    int error = ipod_agent_submit(IPHONE4S_MACHINE(obj)->agent, value);
    if (error) {
        error_setg(errp, "%s", ipod_agent_submit_error(error));
    }
}

static void n94_cancel_agent_request(Object *obj, const char *value, Error **errp)
{
    ipod_agent_cancel(IPHONE4S_MACHINE(obj)->agent, value);
}

static char *n94_get_agent_result(Object *obj, Error **errp)
{
    return ipod_agent_take_result(IPHONE4S_MACHINE(obj)->agent);
}

static char *n94_get_agent_status(Object *obj, Error **errp)
{
    return g_strdup(ipod_agent_status(IPHONE4S_MACHINE(obj)->agent, qemu_clock_get_ms(QEMU_CLOCK_REALTIME)));
}

static void iphone4s_instance_init(Object *obj)
{
    IPhone4SMachineState *s = IPHONE4S_MACHINE(obj);
    guest_pb_init(&s->pb, obj, "iphone4s");
    guest_pkg_init(&s->pkg, obj);
    s->agent = ipod_agent_new();
    ipod_agent_publish(s->agent);
    object_property_add_str(obj, "agent-request", NULL, n94_set_agent_request);
    object_property_add_str(obj, "agent-cancel", NULL, n94_cancel_agent_request);
    object_property_add_str(obj, "agent-result", n94_get_agent_result, NULL);
    object_property_add_str(obj, "agent-status", n94_get_agent_status, NULL);
}

static void iphone4s_instance_finalize(Object *obj)
{
    IPhone4SMachineState *s = IPHONE4S_MACHINE(obj);
    if (s->ae2_timer)
        timer_free(s->ae2_timer);
    if (s->ap_off_timer)
        timer_free(s->ap_off_timer);
    ipod_agent_publish(NULL);
    ipod_agent_free(s->agent);
    g_free(s->kboot);
    g_free(s->nand);
    g_free(s->nand_overlay);
}

static const TypeInfo iphone4s_type = {
    .name = TYPE_IPHONE4S_MACHINE,
    .parent = TYPE_MACHINE,
    .interfaces = arm_machine_interfaces,
    .instance_size = sizeof(IPhone4SMachineState),
    .class_init = iphone4s_class_init,
    .instance_init = iphone4s_instance_init,
    .instance_finalize = iphone4s_instance_finalize,
};

static void iphone4s_register_types(void)
{
    type_register_static(&iphone4s_type);
}
type_init(iphone4s_register_types)
