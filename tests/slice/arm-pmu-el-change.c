/* The PMU's EL-change hooks do nothing while nothing can count: PMCR.E clear and no EL2.
 *
 * iOS takes thousands of exceptions a second with the PMU off, and each ran pmu_op_start/finish (a clock read
 * and a pmu_counter_enabled per counter). With PMCR.E clear and no EL2 (whose MDCR_EL2.HPME enables the
 * counters past HPMN by itself) no counter is enabled, so op_start only refreshes deltas that the PMCR write
 * setting E refreshes again (pmcr_write runs op_start before the write). With E set, or with EL2, the hooks
 * run as before.
 *
 * Mutation (named, must fail this test): pmu_counting_off returns false.
 *
 * SLICE target/arm/cpregs-pmu.c fn pmu_counting_off pmu_pre_el_change pmu_post_el_change
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define PMCRE 1
enum { ARM_FEATURE_EL2 };
typedef struct { struct { uint64_t c9_pmcr; } cp15; bool el2; } CPUARMState;
typedef struct { CPUARMState env; } ARMCPU;

static int starts, finishes;
static bool arm_feature(CPUARMState *env, int f) { return env->el2; }
static void pmu_op_start(CPUARMState *env) { starts++; }
static void pmu_op_finish(CPUARMState *env) { finishes++; }

#include "slice.h"

/* One exception taken and returned from: the hooks' calls of op_start and op_finish. */
static int el_round_trip(uint64_t pmcr, bool el2)
{
    ARMCPU cpu = { { { pmcr }, el2 } };

    starts = finishes = 0;
    pmu_pre_el_change(&cpu, NULL);
    pmu_post_el_change(&cpu, NULL);
    pmu_pre_el_change(&cpu, NULL);
    pmu_post_el_change(&cpu, NULL);
    assert(starts == finishes);
    return starts;
}

int main(void)
{
    assert(el_round_trip(0, false) == 0);           /* the PMU off: nothing to do */
    assert(el_round_trip(0x41 << 24, false) == 0);  /* PMCR's other bits don't count */
    assert(el_round_trip(PMCRE, false) == 2);       /* counting: each EL change runs both */
    assert(el_round_trip(0, true) == 2);            /* EL2's HPME can count with E clear */
    puts("PASS: PMU EL-change hooks skip work only with PMCR.E clear and no EL2");
}
