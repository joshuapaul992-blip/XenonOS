/*
 * HAL - time source for early boot.
 *
 * The TSC frequency is measured against PIT channel 2 (the PC speaker
 * timer, which can be read without interrupts). Phase 1.1 adds HPET and the
 * APIC timer; until then this gives the log its timestamps.
 */
#include "hal.h"

#define PIT_HZ        1193182u
#define CALIBRATE_MS  20u

static uint64_t g_tsc_hz;
static uint64_t g_tsc_boot;     /* TSC value at g_ns_base */
static uint64_t g_ns_base;      /* nanoseconds since boot at g_tsc_boot */

uint64_t HalCalibrateTsc(void)
{
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    if (a >= 0x15) {
        /* Some CPUs report the TSC frequency directly. */
        uint32_t den, num, crystal, unused;
        cpuid(0x15, 0, &den, &num, &crystal, &unused);
        if (den && num && crystal) {
            g_tsc_hz = (uint64_t)crystal * num / den;
            g_tsc_boot = rdtsc();
            return g_tsc_hz;
        }
    }

    uint16_t count = (uint16_t)(PIT_HZ * CALIBRATE_MS / 1000);
    uint8_t port61 = inb(0x61);
    outb(0x61, (port61 & ~0x02) | 0x01);   /* gate on, speaker off */
    outb(0x43, 0xB0);                      /* channel 2, lo/hi, mode 0 */
    outb(0x42, count & 0xFF);
    outb(0x42, count >> 8);

    uint64_t start = rdtsc();
    uint64_t guard = 0;
    while (!(inb(0x61) & 0x20) && ++guard < 100000000ull)
        ;
    uint64_t end = rdtsc();
    outb(0x61, port61);

    g_tsc_hz = (end - start) * 1000 / CALIBRATE_MS;
    if (g_tsc_hz < 100000000ull)           /* nonsense: assume 2 GHz */
        g_tsc_hz = 2000000000ull;
    g_tsc_boot = start;
    return g_tsc_hz;
}

uint64_t HalGetTscHz(void)
{
    return g_tsc_hz;
}

uint64_t HalGetTimeNs(void)
{
    if (!g_tsc_hz)
        return 0;
    uint64_t t = rdtsc() - g_tsc_boot;
    return g_ns_base + (t / g_tsc_hz) * 1000000000ull + (t % g_tsc_hz) * 1000000000ull / g_tsc_hz;
}

/* Switch to a more accurate TSC frequency without making the clock jump. */
void HalTscSetFrequency(uint64_t hz)
{
    uint64_t flags = read_rflags();
    cpu_cli();
    uint64_t now_ns = HalGetTimeNs();
    g_tsc_boot = rdtsc();
    g_ns_base = now_ns;
    g_tsc_hz = hz;
    if (flags & (1u << 9))
        cpu_sti();
}

void HalStallMs(unsigned ms)
{
    uint64_t end = HalGetTimeNs() + (uint64_t)ms * 1000000ull;
    while (HalGetTimeNs() < end)
        cpu_pause();
}
