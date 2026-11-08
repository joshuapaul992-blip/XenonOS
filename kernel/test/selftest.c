/* SPDX-License-Identifier: MIT */
/*
 * Kernel self-tests, run at boot when the command line has "xenon.selftest".
 *
 * Each test prints "SELFTEST: <name> PASS" or "SELFTEST: <name> FAIL: why",
 * and a summary line at the end that tools/test.sh checks.
 */
#include "xenon.h"
#include "mm.h"
#include "ke.h"
#include "hal.h"
#include "rtl.h"
#include "console.h"
#include "loader.h"

static unsigned g_pass, g_fail;
char KiSelfTestWhy[160];
#define g_why KiSelfTestWhy

#define CHECK(cond, ...) do { if (!(cond)) { snprintf(g_why, sizeof(g_why), __VA_ARGS__); return false; } } while (0)

static void run(const char *name, bool (*fn)(void))
{
    g_why[0] = 0;
    uint64_t t0 = HalGetTimeNs();
    bool ok = fn();
    uint64_t ms = (HalGetTimeNs() - t0) / 1000000;
    if (ok) {
        g_pass++;
        DbgPrint("SELFTEST: %s PASS (%llu ms)", name, (unsigned long long)ms);
    } else {
        g_fail++;
        DbgPrint("SELFTEST: %s FAIL: %s", name, g_why);
    }
}

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static void spin_ms(unsigned ms)
{
    uint64_t end = HalGetTimeNs() + (uint64_t)ms * 1000000;
    while (HalGetTimeNs() < end)
        cpu_pause();
}

/* ---- Physical memory ---------------------------------------------------- */

static bool test_pmm(void)
{
    MM_STATS before, after;
    MmQueryStats(&before);

    uint64_t a = MmAllocatePhysicalPages(1, MM_ANY_ADDRESS, 0);
    CHECK(a && !(a & 0xFFF), "single page allocation failed");
    uint64_t *p = MmPhysToVirt(a);
    for (int i = 0; i < 512; i++)
        CHECK(p[i] == 0, "page not zeroed");

    uint64_t big = MmAllocatePhysicalPages(64, MM_ANY_ADDRESS, 65536);
    CHECK(big && (big & 0xFFFF) == 0, "64 aligned pages: got 0x%llx", (unsigned long long)big);

    uint64_t low = MmAllocatePhysicalPages(4, MM_BELOW_16M, 0);
    CHECK(low && low + 4 * PAGE_SIZE <= MM_BELOW_16M, "below-16M allocation: 0x%llx", (unsigned long long)low);

    uint64_t dma32 = MmAllocatePhysicalPages(16, MM_BELOW_4G, 0);
    CHECK(dma32 && dma32 + 16 * PAGE_SIZE <= MM_BELOW_4G, "below-4G allocation: 0x%llx", (unsigned long long)dma32);

    uint64_t isa = MmAllocateIsaDmaBuffer(65536);
    CHECK(isa && isa + 65536 <= MM_BELOW_16M && (isa >> 16) == ((isa + 65535) >> 16),
          "ISA DMA buffer 0x%llx crosses 64K or is above 16M", (unsigned long long)isa);
    uint64_t isa2 = MmAllocateIsaDmaBuffer(18 * 512);       /* one floppy track */
    CHECK(isa2 && (isa2 >> 16) == ((isa2 + 18 * 512 - 1) >> 16), "small ISA buffer crosses 64K");

    /* The pages must all be different. */
    CHECK(a != big && a != low && low != dma32 && isa != isa2, "allocator returned overlapping pages");

    MmFreePhysicalPages(a, 1);
    MmFreePhysicalPages(big, 64);
    MmFreePhysicalPages(low, 4);
    MmFreePhysicalPages(dma32, 16);
    MmFreePhysicalPages(isa, 16);
    MmFreePhysicalPages(isa2, BYTES_TO_PAGES(18 * 512));

    MmQueryStats(&after);
    CHECK(after.FreePages == before.FreePages, "leak: %llu free pages before, %llu after",
          (unsigned long long)before.FreePages, (unsigned long long)after.FreePages);
    return true;
}

/* ---- Virtual memory ----------------------------------------------------- */

static bool test_vmm(void)
{
    uint64_t pa = MmAllocatePhysicalPages(1, MM_ANY_ADDRESS, 0);
    CHECK(pa, "out of memory");
    volatile uint32_t *m = MmMapIoSpace(pa + 0x10, 64, MmCached);
    CHECK(m, "MmMapIoSpace failed");
    CHECK(((uint64_t)m & 0xFFF) == 0x10, "page offset lost");
    m[0] = 0xC0FFEE11;
    CHECK(*(volatile uint32_t *)((uint8_t *)MmPhysToVirt(pa) + 0x10) == 0xC0FFEE11,
          "write through the new mapping not seen in the direct map");
    CHECK(MmGetPhysicalAddress((void *)m) == pa + 0x10, "MmGetPhysicalAddress mismatch");
    MmUnmapIoSpace((void *)m, 64);
    CHECK(MmGetPhysicalAddress((void *)m) == 0, "page still mapped after unmap");

    /* The kernel image and the direct map must translate correctly. */
    extern char __text_start[];
    CHECK(MmGetPhysicalAddress(__text_start) != 0, "kernel text not mapped");
    CHECK(MmGetPhysicalAddress(MmPhysToVirt(pa)) == pa, "direct map translation wrong");
    MmFreePhysicalPages(pa, 1);

    /* A kernel stack has a guard page below it. */
    uint8_t *top = MmCreateKernelStack(16384);
    CHECK(top, "MmCreateKernelStack failed");
    CHECK(MmGetPhysicalAddress(top - 16384) != 0, "stack bottom not mapped");
    CHECK(MmGetPhysicalAddress(top - 16384 - 1) == 0, "guard page is mapped");
    MmFreeKernelStack(top, 16384);
    CHECK(MmGetPhysicalAddress(top - 16384) == 0, "stack still mapped after MmFreeKernelStack");

    /* Every RAM region (including any above 4 GiB) must be in the direct map. */
    for (uint32_t i = 0; i < KeLoaderBlock.MemoryMapCount; i++) {
        LOADER_MEMORY_DESCRIPTOR *m = &KeLoaderBlock.MemoryMap[i];
        if (m->Type != MEM_USABLE || m->Length < PAGE_SIZE)
            continue;
        uint64_t first = ALIGN_UP(m->Base, PAGE_SIZE), last = ALIGN_DOWN(m->Base + m->Length, PAGE_SIZE) - PAGE_SIZE;
        CHECK(MmGetPhysicalAddress(MmPhysToVirt(first)) == first && MmGetPhysicalAddress(MmPhysToVirt(last)) == last,
              "RAM region 0x%llx-0x%llx not in the direct map", (unsigned long long)m->Base,
              (unsigned long long)(m->Base + m->Length));
    }

    uint64_t efer = HalReadMsr(0xC0000080);
    CHECK(efer & (1u << 11), "no-execute (EFER.NXE) not enabled");
    CHECK(read_cr0() & (1u << 16), "CR0.WP not set");
    return true;
}

/* ---- Pool --------------------------------------------------------------- */

#define SLOTS 512
static struct { uint8_t *p; uint32_t size; uint8_t fill; } g_slots[SLOTS];

static bool test_pool(void)
{
    POOL_STATS ps0, ps1;
    MM_STATS ms0, ms1;
    ExQueryPoolStats(&ps0);
    MmQueryStats(&ms0);

    for (int op = 0; op < 40000; op++) {
        unsigned i = rnd() % SLOTS;
        if (g_slots[i].p) {
            for (uint32_t j = 0; j < g_slots[i].size; j++)
                CHECK(g_slots[i].p[j] == g_slots[i].fill, "block %u corrupted at byte %u (op %d)", i, j, op);
            ExFreePoolWithTag(g_slots[i].p, 'Test');
            g_slots[i].p = NULL;
        } else {
            uint64_t r = rnd();
            uint32_t size = (r & 0xF) == 0 ? (uint32_t)(r >> 8) % 40000 + 1    /* some big blocks */
                                           : (uint32_t)(r >> 8) % 2000 + 1;
            uint8_t *p = ExAllocatePoolWithTag(NonPagedPool, size, 'Test');
            CHECK(p, "allocation of %u bytes failed (op %d)", size, op);
            CHECK(((uint64_t)p & 15) == 0, "block not 16-byte aligned");
            g_slots[i].p = p;
            g_slots[i].size = size;
            g_slots[i].fill = (uint8_t)(i * 7 + op);
            memset(p, g_slots[i].fill, size);
        }
    }
    for (unsigned i = 0; i < SLOTS; i++) {
        if (g_slots[i].p) {
            ExFreePoolWithTag(g_slots[i].p, 'Test');
            g_slots[i].p = NULL;
        }
    }

    uint8_t *z = ExAllocatePoolWithTag(NonPagedPoolZeroed, 1000, 'Test');
    CHECK(z, "zeroed allocation failed");
    for (int i = 0; i < 1000; i++)
        CHECK(z[i] == 0, "NonPagedPoolZeroed block not zeroed");
    ExFreePoolWithTag(z, 'Test');

    ExQueryPoolStats(&ps1);
    MmQueryStats(&ms1);
    CHECK(ps1.LiveAllocations == ps0.LiveAllocations && ps1.LiveBytes == ps0.LiveBytes,
          "pool leak: %llu live allocations before, %llu after",
          (unsigned long long)ps0.LiveAllocations, (unsigned long long)ps1.LiveAllocations);
    /* Large blocks go back to the page allocator; small-block pages stay cached in the pool. */
    uint64_t grown = ps1.PagesInPool - ps0.PagesInPool;
    CHECK(ms1.FreePages + grown == ms0.FreePages, "page leak: %lld pages unaccounted for",
          (long long)(ms0.FreePages - ms1.FreePages - grown));
    POOL_TAG_INFO tags[64];
    unsigned n = ExQueryPoolTags(tags, 64);
    for (unsigned i = 0; i < n; i++)
        CHECK(tags[i].Tag != 'Test', "tag 'Test' still has %u live allocations", tags[i].Allocations);
    return true;
}

/* ---- Clock and interrupts ----------------------------------------------- */

static bool test_clock(void)
{
    CHECK(read_rflags() & (1u << 9), "interrupts are disabled");
    uint64_t t0 = KeTickCount;
    uint64_t n0 = HalGetTimeNs();
    spin_ms(200);
    uint64_t ticks = KeTickCount - t0;
    uint64_t ns = HalGetTimeNs() - n0;
    uint64_t expect = ns / (1000000000ull / KE_CLOCK_HZ);
    CHECK(ticks * 100 >= expect * 85 && ticks * 100 <= expect * 115,
          "%llu clock ticks in %llu ms, expected about %llu", (unsigned long long)ticks,
          (unsigned long long)(ns / 1000000), (unsigned long long)expect);

    if (HalHpetPresent()) {
        uint64_t h0 = HalHpetReadNs(), c0 = HalGetTimeNs();
        spin_ms(100);
        uint64_t dh = HalHpetReadNs() - h0, dc = HalGetTimeNs() - c0;
        int64_t diff = (int64_t)dh - (int64_t)dc;
        if (diff < 0) diff = -diff;
        CHECK((uint64_t)diff * 100 <= dh * 2, "TSC and HPET disagree: %llu vs %llu us over 100 ms",
              (unsigned long long)(dc / 1000), (unsigned long long)(dh / 1000));
    }
    return true;
}

static volatile uint64_t g_pit_irqs;
static void pit_isr(KTRAP_FRAME *f, void *ctx) { UNUSED(f); UNUSED(ctx); g_pit_irqs++; }

static bool test_ioapic(void)
{
    CHECK(HalIoApicPinCount() > 0, "no IOAPIC");
    uint8_t vec = KeAllocateVector();
    CHECK(vec, "no free vector");
    KeConnectInterrupt(vec, pit_isr, NULL);
    g_pit_irqs = 0;

    /* PIT channel 0, mode 2 (rate generator), 1000 Hz, routed through the
       IOAPIC with the MADT's override for ISA IRQ 0. */
    uint16_t div = 1193;
    outb(0x43, 0x34);
    outb(0x40, div & 0xFF);
    outb(0x40, div >> 8);
    bool routed = HalEnableIsaIrq(0, vec);
    spin_ms(200);
    HAL_IRQ_OVERRIDE o = HalAcpiIsaIrq(0);
    HalDisableGsi(o.Gsi);
    outb(0x43, 0x30);              /* back to mode 0 so it stops firing */
    outb(0x40, 0);
    outb(0x40, 0);
    KeDisconnectInterrupt(vec);

    CHECK(routed, "could not route ISA IRQ 0 (GSI %u)", o.Gsi);
    CHECK(g_pit_irqs >= 160 && g_pit_irqs <= 240, "%llu PIT interrupts in 200 ms through GSI %u, expected ~200",
          (unsigned long long)g_pit_irqs, o.Gsi);
    return true;
}

/* ---- RTC and dates ------------------------------------------------------ */

static bool test_dates(void)
{
    static const struct { TIME_FIELDS t; int64_t secs; uint8_t wd; } cases[] = {
        { { 1970, 1, 1, 0, 0, 0, 0 }, 0, 4 },
        { { 2000, 2, 29, 12, 0, 0, 0 }, 951825600, 2 },
        { { 2000, 3, 1, 0, 0, 0, 0 }, 951868800, 3 },
        { { 2026, 11, 8, 0, 0, 0, 0 }, 1794096000, 0 },
        { { 2038, 1, 19, 3, 14, 8, 0 }, 2147483648ll, 2 },
        { { 2100, 3, 1, 0, 0, 0, 0 }, 4107542400ll, 1 },
    };
    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        int64_t u = RtlTimeFieldsToUnix(&cases[i].t);
        CHECK(u == cases[i].secs, "%u-%02u-%02u -> %lld, expected %lld", cases[i].t.Year, cases[i].t.Month,
              cases[i].t.Day, (long long)u, (long long)cases[i].secs);
        TIME_FIELDS back;
        RtlUnixToTimeFields(u, &back);
        CHECK(back.Year == cases[i].t.Year && back.Month == cases[i].t.Month && back.Day == cases[i].t.Day &&
              back.Hour == cases[i].t.Hour && back.Minute == cases[i].t.Minute && back.Second == cases[i].t.Second,
              "round trip of %lld failed", (long long)u);
        CHECK(back.Weekday == cases[i].wd, "weekday of %lld is %u, expected %u", (long long)u, back.Weekday,
              cases[i].wd);
    }
    return true;
}

static bool test_rtc(void)
{
    TIME_FIELDS a, b;
    CHECK(HalQueryRealTimeClock(&a), "RTC read failed");
    CHECK(a.Year >= 2024 && a.Year < 2100, "implausible year %u", a.Year);
    spin_ms(1100);
    CHECK(HalQueryRealTimeClock(&b), "second RTC read failed");
    int64_t d = RtlTimeFieldsToUnix(&b) - RtlTimeFieldsToUnix(&a);
    CHECK(d >= 1 && d <= 2, "RTC advanced %lld s in 1.1 s", (long long)d);
    return true;
}

/* ---- PCI ---------------------------------------------------------------- */

static bool test_pci(void)
{
    CHECK(HalPciDeviceCount() > 0, "no PCI devices");
    HAL_PCI_DEVICE *host = HalPciGetDevice(0);
    CHECK(host->Bus == 0 && host->Device == 0 && host->ClassCode == 0x06 && host->SubClass == 0x00,
          "00:00.0 is not a host bridge (class %02x:%02x)", host->ClassCode, host->SubClass);
    /* Config reads through the HAL must match the cached IDs. */
    for (unsigned i = 0; i < HalPciDeviceCount(); i++) {
        HAL_PCI_DEVICE *d = HalPciGetDevice(i);
        CHECK(HalPciRead16(d, 0) == d->VendorId && HalPciRead16(d, 2) == d->DeviceId,
              "config read mismatch at %02x:%02x.%u", d->Bus, d->Device, d->Function);
    }
    return true;
}

bool KiTestIrqlDpc(void);
bool KiTestEvents(void);
bool KiTestMutex(void);
bool KiTestSemaphore(void);
bool KiTestTimers(void);
bool KiTestWaits(void);
bool KiTestPreemption(void);
bool KiTestRoundRobin(void);
bool KiTestThreads100(void);
bool KiTestObjects(void);

void KiRunSelfTests(void)
{
    DbgPrint("SELFTEST: starting");
    run("pmm", test_pmm);
    run("vmm", test_vmm);
    run("pool", test_pool);
    run("clock", test_clock);
    run("ioapic", test_ioapic);
    run("dates", test_dates);
    run("rtc", test_rtc);
    run("pci", test_pci);
    run("irql-dpc", KiTestIrqlDpc);
    run("events", KiTestEvents);
    run("mutex", KiTestMutex);
    run("semaphore", KiTestSemaphore);
    run("timers", KiTestTimers);
    run("waits", KiTestWaits);
    run("preemption", KiTestPreemption);
    run("round-robin", KiTestRoundRobin);
    run("threads100", KiTestThreads100);
    run("objects", KiTestObjects);
    DbgPrint("SELFTEST: %u passed, %u failed", g_pass, g_fail);
}
