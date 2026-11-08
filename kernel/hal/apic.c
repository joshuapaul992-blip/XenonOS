/*
 * HAL - local APIC and IOAPIC.
 *
 * The local APIC is used in xAPIC mode (memory-mapped registers) unless the
 * firmware has already switched it to x2APIC mode, in which case the
 * registers are reached through MSRs. Device interrupts are routed through
 * the IOAPIC(s) described in the ACPI MADT; the legacy 8259 PIC stays masked.
 */
#include "hal.h"
#include "mm.h"
#include "ke.h"
#include "rtl.h"
#include "console.h"

#define MSR_APIC_BASE        0x1B
#define APIC_BASE_ENABLE     (1ull << 11)
#define APIC_BASE_X2APIC     (1ull << 10)

#define LAPIC_ID             0x020
#define LAPIC_VERSION        0x030
#define LAPIC_TPR            0x080
#define LAPIC_EOI            0x0B0
#define LAPIC_SVR            0x0F0
#define LAPIC_ESR            0x280
#define LAPIC_LVT_TIMER      0x320
#define LAPIC_LVT_LINT0      0x350
#define LAPIC_LVT_LINT1      0x360
#define LAPIC_LVT_ERROR      0x370
#define LAPIC_TIMER_INITIAL  0x380
#define LAPIC_TIMER_CURRENT  0x390
#define LAPIC_TIMER_DIVIDE   0x3E0

#define LVT_MASKED           (1u << 16)
#define LVT_TIMER_PERIODIC   (1u << 17)
#define LVT_DELIVERY_NMI     (4u << 8)

#define IOAPIC_REGSEL        0x00
#define IOAPIC_WINDOW        0x10
#define IOAPIC_REG_ID        0x00
#define IOAPIC_REG_VERSION   0x01
#define IOAPIC_REDIR(n)      (0x10 + 2 * (n))

static volatile uint32_t *g_lapic;          /* NULL in x2APIC mode */
static bool g_x2apic;
static uint64_t g_timer_hz;                 /* local APIC timer input / 16 */

static struct {
    volatile uint32_t *Regs;
    uint32_t GsiBase;
    uint32_t Pins;
} g_ioapic[HAL_MAX_IOAPICS];
static unsigned g_ioapic_count;
static KSPIN_LOCK g_ioapic_lock;

/* ---- Local APIC --------------------------------------------------------- */

static uint32_t lapic_read(uint32_t reg)
{
    if (g_x2apic)
        return (uint32_t)HalReadMsr(0x800 + (reg >> 4));
    return g_lapic[reg / 4];
}

static void lapic_write(uint32_t reg, uint32_t v)
{
    if (g_x2apic)
        HalWriteMsr(0x800 + (reg >> 4), v);
    else
        g_lapic[reg / 4] = v;
}

uint32_t HalGetCurrentApicId(void)
{
    uint32_t id = lapic_read(LAPIC_ID);
    return g_x2apic ? id : id >> 24;
}

bool HalApicIsX2(void)
{
    return g_x2apic;
}

void HalEndOfInterrupt(void)
{
    lapic_write(LAPIC_EOI, 0);
}

uint64_t HalApicTimerFrequency(void)
{
    return g_timer_hz;
}

/* Measure the timer's count rate. 'wait_ms' busy-waits using a reference clock. */
void HalApicTimerCalibrate(void (*wait_ms)(unsigned), unsigned ms)
{
    lapic_write(LAPIC_TIMER_DIVIDE, 0x3);               /* divide by 16 */
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED | VECTOR_CLOCK);
    lapic_write(LAPIC_TIMER_INITIAL, 0xFFFFFFFF);
    wait_ms(ms);
    uint32_t left = lapic_read(LAPIC_TIMER_CURRENT);
    lapic_write(LAPIC_TIMER_INITIAL, 0);
    g_timer_hz = (uint64_t)(0xFFFFFFFFu - left) * 1000 / ms;
}

void HalApicTimerStartPeriodic(uint8_t vector, uint32_t hz)
{
    uint64_t count = g_timer_hz / hz;
    if (count == 0) count = 1;
    if (count > 0xFFFFFFFF) count = 0xFFFFFFFF;
    lapic_write(LAPIC_TIMER_DIVIDE, 0x3);
    lapic_write(LAPIC_LVT_TIMER, LVT_TIMER_PERIODIC | vector);
    lapic_write(LAPIC_TIMER_INITIAL, (uint32_t)count);
}

/* ---- IOAPIC ------------------------------------------------------------- */

static uint32_t ioapic_read(unsigned i, uint32_t reg)
{
    g_ioapic[i].Regs[IOAPIC_REGSEL / 4] = reg;
    return g_ioapic[i].Regs[IOAPIC_WINDOW / 4];
}

static void ioapic_write(unsigned i, uint32_t reg, uint32_t v)
{
    g_ioapic[i].Regs[IOAPIC_REGSEL / 4] = reg;
    g_ioapic[i].Regs[IOAPIC_WINDOW / 4] = v;
}

static int ioapic_for_gsi(uint32_t gsi, uint32_t *pin)
{
    for (unsigned i = 0; i < g_ioapic_count; i++) {
        if (gsi >= g_ioapic[i].GsiBase && gsi < g_ioapic[i].GsiBase + g_ioapic[i].Pins) {
            *pin = gsi - g_ioapic[i].GsiBase;
            return (int)i;
        }
    }
    return -1;
}

bool HalEnableGsi(uint32_t gsi, uint8_t vector, bool active_low, bool level)
{
    uint32_t pin;
    int i = ioapic_for_gsi(gsi, &pin);
    if (i < 0)
        return false;
    uint32_t lo = vector;                     /* fixed delivery, physical destination */
    if (active_low) lo |= 1u << 13;
    if (level) lo |= 1u << 15;
    uint32_t hi = HalGetCurrentApicId() << 24;
    KIRQL_STATE s = KeAcquireSpinLock(&g_ioapic_lock);
    ioapic_write((unsigned)i, IOAPIC_REDIR(pin) + 1, hi);
    ioapic_write((unsigned)i, IOAPIC_REDIR(pin), lo);
    KeReleaseSpinLock(&g_ioapic_lock, s);
    return true;
}

void HalDisableGsi(uint32_t gsi)
{
    uint32_t pin;
    int i = ioapic_for_gsi(gsi, &pin);
    if (i < 0)
        return;
    KIRQL_STATE s = KeAcquireSpinLock(&g_ioapic_lock);
    ioapic_write((unsigned)i, IOAPIC_REDIR(pin), LVT_MASKED);
    KeReleaseSpinLock(&g_ioapic_lock, s);
}

bool HalEnableIsaIrq(uint8_t irq, uint8_t vector)
{
    HAL_IRQ_OVERRIDE o = HalAcpiIsaIrq(irq);
    return HalEnableGsi(o.Gsi, vector, o.ActiveLow, o.LevelTriggered);
}

unsigned HalIoApicPinCount(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < g_ioapic_count; i++)
        n += g_ioapic[i].Pins;
    return n;
}

/* ---- Initialisation ----------------------------------------------------- */

void HalInitApic(void)
{
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    if (!(d & (1u << 9)))
        KeBugCheckEx(HAL_INITIALIZATION_FAILED, 1, 0, 0, 0);    /* no local APIC */

    uint64_t base = HalReadMsr(MSR_APIC_BASE);
    g_x2apic = (base & APIC_BASE_X2APIC) != 0;
    if (!(base & APIC_BASE_ENABLE))
        HalWriteMsr(MSR_APIC_BASE, base | APIC_BASE_ENABLE);
    if (!g_x2apic) {
        uint64_t pa = HalAcpi.LocalApicPhysical ? HalAcpi.LocalApicPhysical : (base & 0xFFFFFF000ull);
        g_lapic = MmMapIoSpace(pa, PAGE_SIZE, MmNonCached);
        if (!g_lapic)
            KeBugCheckEx(HAL_INITIALIZATION_FAILED, 2, pa, 0, 0);
    }

    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT0, LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT1, LVT_DELIVERY_NMI);
    lapic_write(LAPIC_LVT_ERROR, VECTOR_APIC_ERROR);
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_SVR, 0x100 | VECTOR_SPURIOUS);       /* software enable */
    lapic_write(LAPIC_EOI, 0);

    uint32_t ver = lapic_read(LAPIC_VERSION);
    DbgPrint("HAL: local APIC id %u, version 0x%02x, %u LVT entries, %s mode",
             HalGetCurrentApicId(), ver & 0xFF, ((ver >> 16) & 0xFF) + 1, g_x2apic ? "x2APIC" : "xAPIC");

    for (unsigned i = 0; i < HalAcpi.IoApicCount && g_ioapic_count < HAL_MAX_IOAPICS; i++) {
        volatile uint32_t *regs = MmMapIoSpace(HalAcpi.IoApics[i].Physical, PAGE_SIZE, MmNonCached);
        if (!regs)
            continue;
        unsigned n = g_ioapic_count++;
        g_ioapic[n].Regs = regs;
        g_ioapic[n].GsiBase = HalAcpi.IoApics[i].GsiBase;
        uint32_t v = ioapic_read(n, IOAPIC_REG_VERSION);
        g_ioapic[n].Pins = ((v >> 16) & 0xFF) + 1;
        for (uint32_t pin = 0; pin < g_ioapic[n].Pins; pin++) {
            ioapic_write(n, IOAPIC_REDIR(pin), LVT_MASKED);
            ioapic_write(n, IOAPIC_REDIR(pin) + 1, 0);
        }
        DbgPrint("HAL: IOAPIC id %u at 0x%llx, GSI %u-%u", HalAcpi.IoApics[i].Id,
                 (unsigned long long)HalAcpi.IoApics[i].Physical, g_ioapic[n].GsiBase,
                 g_ioapic[n].GsiBase + g_ioapic[n].Pins - 1);
    }
    if (!g_ioapic_count)
        DbgPrint("HAL: warning: no IOAPIC found - device interrupts are unavailable");
}
