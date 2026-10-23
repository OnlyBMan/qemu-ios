#ifndef HW_CS42L58_H
#define HW_CS42L58_H

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/sysbus.h"
#include "hw/i2c/i2c.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"

#define TYPE_CS42L58                 "cs42l58"
#define TYPE_CS42L63                 "cs42l63"   /* A5 control register map, power-down status in 0x62 */
#define TYPE_CS42L59                 "cs42l59"   /* + power-down status, rate in 0x0a */
OBJECT_DECLARE_SIMPLE_TYPE(CS42L58State, CS42L58)

typedef struct CS42L58State {
	I2CSlave i2c;
	uint32_t cmd;
	bool have_cmd;
	bool autoinc;
	uint8_t regs[128];
	bool cs42l63;
	bool pdn_status;     /* CS42L59: 0x38 bit 3 reports power-down done */
	uint8_t rate_reg;    /* the register whose low five bits select LRCLK */
	Clock *lrclk;
} CS42L58State;

#endif
