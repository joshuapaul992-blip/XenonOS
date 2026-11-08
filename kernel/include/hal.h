/*
 * HAL - hardware abstraction layer.
 */
#pragma once
#include "xenon.h"

/* Serial port (COM1 by default). Used for the debug log and automated tests. */
bool HalSerialInit(void);
bool HalSerialPresent(void);
void HalSerialWrite(const char *s, size_t len);
void HalSerialPutChar(char c);

/* Reboot the machine (keyboard controller, then triple fault). */
NORETURN void HalReboot(void);

/* QEMU/Bochs debug exit: lets automated tests stop the VM. No-op on real hardware. */
void HalDebugExit(uint8_t code);

/* Early time source (TSC calibrated with the PIT). */
uint64_t HalCalibrateTsc(void);
uint64_t HalGetTscHz(void);
uint64_t HalGetTimeNs(void);      /* nanoseconds since calibration */
void     HalStallMs(unsigned ms);

/* ---- ACPI tables (hal/acpi.c) ------------------------------------------- */

typedef struct PACKED {
    char     Signature[4];
    uint32_t Length;
    uint8_t  Revision;
    uint8_t  Checksum;
    char     OemId[6];
    char     OemTableId[8];
    uint32_t OemRevision;
    uint32_t CreatorId;
    uint32_t CreatorRevision;
} ACPI_TABLE_HEADER;

#define HAL_MAX_CPUS     64
#define HAL_MAX_IOAPICS  8
#define HAL_MAX_OVERRIDES 24
#define HAL_MAX_ECAM     8

typedef struct {
    uint8_t  Source;        /* ISA IRQ */
    uint32_t Gsi;           /* global system interrupt */
    bool     ActiveLow;
    bool     LevelTriggered;
} HAL_IRQ_OVERRIDE;

typedef struct {
    uint64_t Base;
    uint16_t Segment;
    uint8_t  StartBus, EndBus;
} HAL_ECAM_REGION;

typedef struct {
    bool     Present;
    uint8_t  Revision;
    char     OemId[7];
    unsigned TableCount;
    /* MADT */
    uint64_t LocalApicPhysical;
    unsigned CpuCount;                       /* enabled or online-capable */
    uint32_t CpuApicIds[HAL_MAX_CPUS];
    bool     HasLegacyPic;
    unsigned IoApicCount;
    struct { uint8_t Id; uint64_t Physical; uint32_t GsiBase; } IoApics[HAL_MAX_IOAPICS];
    unsigned OverrideCount;
    HAL_IRQ_OVERRIDE Overrides[HAL_MAX_OVERRIDES];
    /* FADT */
    bool     HasFadt;
    uint8_t  CenturyRegister;                /* CMOS index, 0 if none */
    uint16_t BootArchFlags;                  /* IAPC_BOOT_ARCH */
    uint32_t PmTimerPort;
    bool     PmTimer32Bit;
    /* HPET */
    uint64_t HpetPhysical;                   /* 0 if none */
    /* MCFG (PCI Express ECAM) */
    unsigned EcamCount;
    HAL_ECAM_REGION Ecam[HAL_MAX_ECAM];
} HAL_ACPI_INFO;

#define ACPI_BOOT_ARCH_LEGACY_DEVICES 0x0001
#define ACPI_BOOT_ARCH_8042           0x0002
#define ACPI_BOOT_ARCH_NO_VGA         0x0004
#define ACPI_BOOT_ARCH_NO_CMOS_RTC    0x0020

extern HAL_ACPI_INFO HalAcpi;
void HalInitAcpi(void);
/* Find the n-th table with this signature (mapped, checksum verified). */
ACPI_TABLE_HEADER *HalAcpiFindTable(const char sig[4], unsigned n);
/* Translate an ISA IRQ through the MADT overrides. */
HAL_IRQ_OVERRIDE HalAcpiIsaIrq(uint8_t irq);

/* ---- Local APIC / IOAPIC (hal/apic.c) ----------------------------------- */

void     HalInitApic(void);
uint32_t HalGetCurrentApicId(void);
void     HalEndOfInterrupt(void);
bool     HalApicIsX2(void);
/* Route an ISA IRQ (0-15) or a GSI to a vector on this CPU. */
bool     HalEnableIsaIrq(uint8_t irq, uint8_t vector);
bool     HalEnableGsi(uint32_t gsi, uint8_t vector, bool active_low, bool level);
void     HalDisableGsi(uint32_t gsi);
unsigned HalIoApicPinCount(void);
/* Local APIC timer */
void     HalApicTimerStartPeriodic(uint8_t vector, uint32_t hz);
uint64_t HalApicTimerFrequency(void);

/* ---- Clocks (hal/hpet.c, hal/timer.c) ----------------------------------- */

bool     HalInitHpet(void);
bool     HalHpetPresent(void);
uint64_t HalHpetReadNs(void);
uint64_t HalHpetFrequency(void);
/* Pick the best clock, recalibrate the TSC against it and start the tick. */
void     HalInitClock(void);
const char *HalClockSourceName(void);
bool     HalTscInvariant(void);

/* ---- CMOS real-time clock (hal/cmos.c) ---------------------------------- */

typedef struct {
    uint16_t Year;      /* 1980.. */
    uint8_t  Month;     /* 1-12 */
    uint8_t  Day;       /* 1-31 */
    uint8_t  Hour, Minute, Second;
    uint8_t  Weekday;   /* 0 = Sunday */
} TIME_FIELDS;

bool     HalQueryRealTimeClock(TIME_FIELDS *t);
bool     HalSetRealTimeClock(const TIME_FIELDS *t);
uint8_t  HalCmosRead(uint8_t index);
/* Seconds since 1970-01-01 00:00:00 for a TIME_FIELDS (no time zones). */
int64_t  RtlTimeFieldsToUnix(const TIME_FIELDS *t);
void     RtlUnixToTimeFields(int64_t secs, TIME_FIELDS *t);

/* ---- PCI (hal/pci.c) ----------------------------------------------------- */

#define HAL_MAX_PCI_DEVICES 256

typedef struct {
    uint16_t Segment;
    uint8_t  Bus, Device, Function;
    uint16_t VendorId, DeviceId;
    uint16_t SubsystemVendorId, SubsystemId;
    uint8_t  ClassCode, SubClass, ProgIf, Revision;
    uint8_t  HeaderType;
    uint8_t  InterruptLine, InterruptPin;
    uint8_t  MsiCap, MsixCap, PcieCap;       /* capability offsets, 0 if absent */
    struct {
        uint64_t Base;
        uint64_t Size;
        bool     Io;
        bool     Prefetchable;
        bool     Is64;
    } Bars[6];
} HAL_PCI_DEVICE;

void     HalInitPci(void);
unsigned HalPciDeviceCount(void);
HAL_PCI_DEVICE *HalPciGetDevice(unsigned index);
/* Find the n-th device with this class/subclass (prog_if 0xFF = any). */
HAL_PCI_DEVICE *HalPciFindClass(uint8_t cls, uint8_t sub, uint8_t prog_if, unsigned n);
uint32_t HalPciRead32(const HAL_PCI_DEVICE *d, uint16_t off);
uint16_t HalPciRead16(const HAL_PCI_DEVICE *d, uint16_t off);
uint8_t  HalPciRead8(const HAL_PCI_DEVICE *d, uint16_t off);
void     HalPciWrite32(const HAL_PCI_DEVICE *d, uint16_t off, uint32_t v);
void     HalPciWrite16(const HAL_PCI_DEVICE *d, uint16_t off, uint16_t v);
void     HalPciWrite8(const HAL_PCI_DEVICE *d, uint16_t off, uint8_t v);
const char *HalPciClassName(uint8_t cls, uint8_t sub, uint8_t prog_if);
const char *HalPciVendorName(uint16_t vendor);
bool     HalPciUsingEcam(void);

/* ---- MSRs ---------------------------------------------------------------- */

static inline uint64_t HalReadMsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void HalWriteMsr(uint32_t msr, uint64_t v)
{
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}
void     HalTscSetFrequency(uint64_t hz);
