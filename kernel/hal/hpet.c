/*
 * HAL - High Precision Event Timer.
 *
 * Phase 1.1 only uses the HPET main counter as an accurate reference to
 * measure the TSC and local APIC timer. Its comparators are left unused.
 */
#include "hal.h"
#include "mm.h"
#include "rtl.h"
#include "console.h"

#define HPET_CAPABILITIES  0x000
#define HPET_CONFIG        0x010
#define HPET_COUNTER       0x0F0
#define HPET_CONFIG_ENABLE 0x1
#define HPET_CONFIG_LEGACY 0x2

static volatile uint64_t *g_hpet;
static uint64_t g_freq;            /* counter ticks per second */
static bool g_64bit;

bool HalInitHpet(void)
{
    if (!HalAcpi.HpetPhysical)
        return false;
    g_hpet = MmMapIoSpace(HalAcpi.HpetPhysical, 1024, MmNonCached);
    if (!g_hpet)
        return false;
    uint64_t cap = g_hpet[HPET_CAPABILITIES / 8];
    uint32_t period_fs = (uint32_t)(cap >> 32);
    if (period_fs == 0 || period_fs > 100000000u) {      /* spec: at most 100 ns */
        g_hpet = NULL;
        return false;
    }
    g_freq = 1000000000000000ull / period_fs;
    g_64bit = (cap >> 13) & 1;
    unsigned timers = ((cap >> 8) & 0x1F) + 1;

    uint64_t cfg = g_hpet[HPET_CONFIG / 8];
    cfg &= ~(uint64_t)HPET_CONFIG_LEGACY;
    g_hpet[HPET_CONFIG / 8] = cfg | HPET_CONFIG_ENABLE;

    DbgPrint("HAL: HPET at 0x%llx, %llu.%03llu MHz, %u timers, %s counter",
             (unsigned long long)HalAcpi.HpetPhysical, (unsigned long long)(g_freq / 1000000),
             (unsigned long long)((g_freq / 1000) % 1000), timers, g_64bit ? "64-bit" : "32-bit");
    return true;
}

bool HalHpetPresent(void)
{
    return g_hpet != NULL;
}

uint64_t HalHpetFrequency(void)
{
    return g_freq;
}

/* Raw counter; 32-bit counters wrap, so callers use wrap-safe differences. */
uint64_t HalHpetReadCounter(void)
{
    return g_64bit ? g_hpet[HPET_COUNTER / 8] : (uint32_t)g_hpet[HPET_COUNTER / 8];
}

bool HalHpetIs64Bit(void)
{
    return g_64bit;
}

uint64_t HalHpetReadNs(void)
{
    uint64_t c = HalHpetReadCounter();
    return (c / g_freq) * 1000000000ull + (c % g_freq) * 1000000000ull / g_freq;
}
