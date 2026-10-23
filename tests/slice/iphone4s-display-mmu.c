/* A5 DisplayPipe has per-layer page tables, sixteen 4 MiB regions per layer.
 * IOVA zero is a valid scanout address. Frames can cross a page boundary.
 * Mutation: bypass the s->a5 translation branch in fb_read.
 * SLICE hw/arm/s5l8930_display.c define DP_UI_BASE
 * SLICE hw/arm/s5l8930_display.c fn fb_read
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MEMTXATTRS_UNSPECIFIED 0
#define MEMTX_OK 0
#define le32_to_cpu(v) (v)
typedef uint64_t hwaddr;
typedef struct {
    uint32_t regs[0x6000 / 4];
} DisplayPipe;
typedef struct {
    bool a5;
    DisplayPipe pipe[1];
} S5L8930DisplayState;
static int address_space_memory;
static uint8_t ram[0x80000];
static hwaddr dart_xlate(S5L8930DisplayState *s, uint32_t va)
{
    return va;
}
static int address_space_read(void *as, hwaddr pa, int attrs, void *out, unsigned n)
{
    assert(pa + n <= sizeof(ram));
    memcpy(out, ram + pa, n);
    return MEMTX_OK;
}
#include "slice.h"
int main(void)
{
    S5L8930DisplayState s = {.a5 = true};
    s.pipe[0].regs[(0x4000 + 0x90) / 4] = 1;
    s.pipe[0].regs[(0x4000 + 0x98) / 4] = 0x10000;
    uint32_t entries[] = {0x30001, 0x40001};
    memcpy(ram + 0x10000, entries, sizeof(entries));
    memset(ram + 0x30000, 0x31, 4096);
    memset(ram + 0x40000, 0x42, 4096);
    uint8_t bytes[20];
    fb_read(&s, 0, 4090, bytes, sizeof(bytes));
    for (unsigned i = 0; i < sizeof(bytes); i++)
        assert(bytes[i] == (i < 6 ? 0x31 : 0x42));
    fb_read(&s, 0, 0, bytes, sizeof(bytes));
    assert(bytes[0] == 0x31);
    s.pipe[0].regs[(0x5000 + 0x90) / 4] = 1;
    s.pipe[0].regs[(0x5000 + 0x98) / 4] = 0x11000;
    uint32_t entry = 0x50001;
    memcpy(ram + 0x11000, &entry, 4);
    memset(ram + 0x50000, 0x53, 4096);
    fb_read(&s, 1, 0, bytes, sizeof(bytes));
    assert(bytes[0] == 0x53);
    fb_read(&s, 0, 0x4000000, bytes, sizeof(bytes));
    for (unsigned i = 0; i < sizeof(bytes); i++)
        assert(bytes[i] == 0);
    fb_read(&s, 0, 0x2000, bytes, sizeof(bytes));
    assert(bytes[0] == 0);
    return 0;
}
