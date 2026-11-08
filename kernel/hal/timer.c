/*
 * HAL - system clock setup.
 *
 * The TSC is the kernel's time source. At boot it was measured roughly
 * against the PIT; here it is measured again against the best reference
 * available (HPET, then the ACPI PM timer, then the PIT), and the local APIC
 * timer is measured against the same reference and started as the periodic
 * system clock tick (KE_CLOCK_HZ).
 */
#include "hal.h"
#include "ke.h"
#include "rtl.h"
#include "console.h"

#define PM_TIMER_HZ     3579545u
#define PIT_HZ          1193182u
#define CALIBRATE_MS    50u

uint64_t HalHpetReadCounter(void);
bool     HalHpetIs64Bit(void);
void     HalApicTimerCalibrate(void (*wait_ms)(unsigned), unsigned ms);

static const char *g_source = "TSC (PIT calibrated)";
static bool g_invariant;

static void wait_hpet(unsigned ms)
{
    uint64_t mask = HalHpetIs64Bit() ? ~0ull : 0xFFFFFFFFull;
    uint64_t target = HalHpetFrequency() * ms / 1000;
    uint64_t start = HalHpetReadCounter();
    while (((HalHpetReadCounter() - start) & mask) < target)
        cpu_pause();
}

static void wait_pmtimer(unsigned ms)
{
    uint32_t mask = HalAcpi.PmTimer32Bit ? 0xFFFFFFFFu : 0xFFFFFFu;
    uint64_t target = (uint64_t)PM_TIMER_HZ * ms / 1000;
    uint32_t last = inl((uint16_t)HalAcpi.PmTimerPort) & mask;
    uint64_t elapsed = 0;
    while (elapsed < target) {
        uint32_t now = inl((uint16_t)HalAcpi.PmTimerPort) & mask;
        elapsed += (now - last) & mask;
        last = now;
    }
}

static void wait_pit(unsigned ms)
{
    while (ms) {
        unsigned chunk = ms > 50 ? 50 : ms;
        uint16_t count = (uint16_t)(PIT_HZ * chunk / 1000);
        uint8_t port61 = inb(0x61);
        outb(0x61, (port61 & ~0x02) | 0x01);
        outb(0x43, 0xB0);
        outb(0x42, count & 0xFF);
        outb(0x42, count >> 8);
        while (!(inb(0x61) & 0x20))
            cpu_pause();
        outb(0x61, port61);
        ms -= chunk;
    }
}

void HalInitClock(void)
{
    void (*wait)(unsigned) = wait_pit;
    const char *ref = "PIT";
    if (HalInitHpet()) {
        wait = wait_hpet;
        ref = "HPET";
    } else if (HalAcpi.PmTimerPort) {
        wait = wait_pmtimer;
        ref = "ACPI PM timer";
    }

    uint32_t a, b, c, d;
    cpuid(0x80000000, 0, &a, &b, &c, &d);
    if (a >= 0x80000007) {
        cpuid(0x80000007, 0, &a, &b, &c, &d);
        g_invariant = (d >> 8) & 1;
    }

    uint64_t flags = read_rflags();
    cpu_cli();
    uint64_t t0 = rdtsc();
    wait(CALIBRATE_MS);
    uint64_t t1 = rdtsc();
    HalTscSetFrequency((t1 - t0) * 1000 / CALIBRATE_MS);
    HalApicTimerCalibrate(wait, CALIBRATE_MS);
    if (flags & (1u << 9))
        cpu_sti();

    static char name[48];
    snprintf(name, sizeof(name), "TSC (%s calibrated)", ref);
    g_source = name;

    DbgPrint("HAL: clock %s, TSC %llu.%03llu MHz%s, APIC timer %llu.%03llu MHz",
             g_source, (unsigned long long)(HalGetTscHz() / 1000000),
             (unsigned long long)((HalGetTscHz() / 1000) % 1000), g_invariant ? " (invariant)" : "",
             (unsigned long long)(HalApicTimerFrequency() / 1000000),
             (unsigned long long)((HalApicTimerFrequency() / 1000) % 1000));

    KeConnectInterrupt(VECTOR_CLOCK, KiClockInterrupt, NULL);
    HalApicTimerStartPeriodic(VECTOR_CLOCK, KE_CLOCK_HZ);
}

const char *HalClockSourceName(void)
{
    return g_source;
}

bool HalTscInvariant(void)
{
    return g_invariant;
}
