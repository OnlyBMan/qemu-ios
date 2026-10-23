/* CS42L63 uses the codec MAP protocol, revision in 0x05, and PDN_DONE in 0x62.
 * SLICE include/hw/arm/ipod_touch_cs42l58.h range typedef struct CS42L58State { | #endif
 * SLICE hw/arm/ipod_touch_cs42l58.c define CS42L58_MAP_|CS42L58_REG_|CS42L59_
 * SLICE hw/arm/ipod_touch_cs42l58.c fn cs42l58_sample_rate cs42l58_event cs42l58_send cs42l58_recv cs42l58_reset
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int I2CSlave, DeviceState, Clock;
typedef struct CS42L58State CS42L58State;
#define CS42L58(s) ((CS42L58State *)(s))
#define TRACE_PRINTF(...) ((void)0)
enum i2c_event { I2C_START_SEND };
static bool codec_trace(void)
{
    return false;
}
static void clock_update_hz(Clock *clock, unsigned rate)
{
}
#include "slice.h"
static void wr(CS42L58State *s, unsigned reg, unsigned value)
{
    cs42l58_event((I2CSlave *)s, I2C_START_SEND);
    cs42l58_send((I2CSlave *)s, reg);
    cs42l58_send((I2CSlave *)s, value);
}
static unsigned rd(CS42L58State *s, unsigned reg)
{
    cs42l58_event((I2CSlave *)s, I2C_START_SEND);
    cs42l58_send((I2CSlave *)s, reg);
    return cs42l58_recv((I2CSlave *)s);
}
int main(void)
{
    CS42L58State s = {.cs42l63 = true, .rate_reg = 0xff};
    cs42l58_reset((DeviceState *)&s);
    assert(rd(&s, 5) == 1);
    wr(&s, 5, 0xff);
    assert(rd(&s, 5) == 1);
    wr(&s, 1, 0x34);
    assert(rd(&s, 1) == 0x34);
    wr(&s, 6, 0);
    assert(!(rd(&s, 0x62) & 0x80));
    wr(&s, 6, 1);
    assert(rd(&s, 0x62) & 0x80);
    wr(&s, 0x88, 0x12);
    cs42l58_send((I2CSlave *)&s, 0x34);
    assert(rd(&s, 8) == 0x12 && rd(&s, 9) == 0x34);
    assert(rd(&s, 0x77) == 0 && rd(&s, 0x78) == 0);
    return 0;
}
