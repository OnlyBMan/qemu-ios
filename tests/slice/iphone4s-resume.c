/* Warm boot consumes the PMU and DRAM suspend markers and enters XNU's retained trampoline.
 * SLICE hw/arm/iphone4s.c fn n94_resume
 */
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#define N94_DRAM_BASE 0x80000000ULL
#define MEMTXATTRS_UNSPECIFIED 0
#define CPU(x) (x)
#define le32_to_cpu(x) (x)
typedef struct {
    struct {
        uint32_t regs[16];
    } env;
} ARMCPU;
typedef struct {
    ARMCPU *cpu;
    void *pmu;
} IPhone4SMachineState;
static int address_space_memory, calls;
static bool suspended;
static uint32_t retained[2];
static void address_space_read(void *as, uint64_t addr, int attrs, void *buf, unsigned size)
{
    assert(addr == N94_DRAM_BASE + 0x80 && size == 8);
    memcpy(buf, retained, 8);
}
static void address_space_set(void *as, uint64_t addr, int v, unsigned size, int attrs)
{
    assert(addr == N94_DRAM_BASE + 0x80 && size == 8);
    memset(retained, v, 8);
}
static bool s5l8940_d1881_take_suspend(void *pmu)
{
    calls++;
    bool value = suspended;
    suspended = false;
    return value;
}
static void cpu_set_pc(ARMCPU *cpu, uint32_t pc)
{
    cpu->env.regs[15] = pc;
}
#include "slice.h"
int main(void)
{
    ARMCPU cpu = {.env.regs = {123}};
    cpu.env.regs[15] = 0x80001000;
    IPhone4SMachineState s = {.cpu = &cpu};
    suspended = true;
    assert(!n94_resume(&s) && calls == 0 && suspended && cpu.env.regs[0] == 123);
    retained[0] = 0x4d4f5358;
    retained[1] = 0x53555350;
    suspended = false;
    assert(!n94_resume(&s) && retained[0] && cpu.env.regs[15] == 0x80001000);
    suspended = true;
    assert(n94_resume(&s) && !retained[0] && !retained[1] && cpu.env.regs[0] == 0 && cpu.env.regs[15] == 0);
    assert(!n94_resume(&s));
    return 0;
}
