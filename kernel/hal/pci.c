/* SPDX-License-Identifier: MIT */
/*
 * HAL - PCI and PCI Express enumeration.
 *
 * Configuration space is reached through ECAM (memory-mapped, from the ACPI
 * MCFG table) when available, otherwise through the legacy 0xCF8/0xCFC
 * ports. Buses are found by walking from bus 0 through PCI-to-PCI bridges.
 * For every function the BARs are sized and the MSI, MSI-X and PCI Express
 * capabilities are located, ready for the drivers that come later.
 */
#include "hal.h"
#include "mm.h"
#include "ke.h"
#include "rtl.h"
#include "console.h"

static HAL_PCI_DEVICE g_devices[HAL_MAX_PCI_DEVICES];
static unsigned g_count;
static bool g_ecam;
static KSPIN_LOCK g_lock;
static volatile uint8_t *g_bus_map[HAL_MAX_ECAM][256];   /* lazily mapped 1 MiB per bus */
static uint8_t g_scanned[256 / 8];

/* ---- Config space access ------------------------------------------------ */

static volatile uint8_t *ecam_ptr(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off)
{
    for (unsigned i = 0; i < HalAcpi.EcamCount; i++) {
        HAL_ECAM_REGION *r = &HalAcpi.Ecam[i];
        if (r->Segment != seg || bus < r->StartBus || bus > r->EndBus)
            continue;
        if (!g_bus_map[i][bus]) {
            uint64_t pa = r->Base + ((uint64_t)(bus - r->StartBus) << 20);
            g_bus_map[i][bus] = MmMapIoSpace(pa, 1u << 20, MmNonCached);
            if (!g_bus_map[i][bus])
                return NULL;
        }
        return g_bus_map[i][bus] + ((uint32_t)dev << 15) + ((uint32_t)fn << 12) + off;
    }
    return NULL;
}

static uint32_t cfg_read32(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off)
{
    if (g_ecam) {
        volatile uint8_t *p = ecam_ptr(seg, bus, dev, fn, off & 0xFFC);
        return p ? *(volatile uint32_t *)p : 0xFFFFFFFF;
    }
    if (seg || off >= 256)
        return 0xFFFFFFFF;
    KIRQL_STATE s = KeAcquireSpinLock(&g_lock);
    outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | (off & 0xFC));
    uint32_t v = inl(0xCFC);
    KeReleaseSpinLock(&g_lock, s);
    return v;
}

static void cfg_write32(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off, uint32_t v)
{
    if (g_ecam) {
        volatile uint8_t *p = ecam_ptr(seg, bus, dev, fn, off & 0xFFC);
        if (p)
            *(volatile uint32_t *)p = v;
        return;
    }
    if (seg || off >= 256)
        return;
    KIRQL_STATE s = KeAcquireSpinLock(&g_lock);
    outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | (off & 0xFC));
    outl(0xCFC, v);
    KeReleaseSpinLock(&g_lock, s);
}

uint32_t HalPciRead32(const HAL_PCI_DEVICE *d, uint16_t off)
{
    return cfg_read32(d->Segment, d->Bus, d->Device, d->Function, off);
}

uint16_t HalPciRead16(const HAL_PCI_DEVICE *d, uint16_t off)
{
    return (uint16_t)(HalPciRead32(d, off & ~3) >> ((off & 2) * 8));
}

uint8_t HalPciRead8(const HAL_PCI_DEVICE *d, uint16_t off)
{
    return (uint8_t)(HalPciRead32(d, off & ~3) >> ((off & 3) * 8));
}

void HalPciWrite32(const HAL_PCI_DEVICE *d, uint16_t off, uint32_t v)
{
    cfg_write32(d->Segment, d->Bus, d->Device, d->Function, off, v);
}

void HalPciWrite16(const HAL_PCI_DEVICE *d, uint16_t off, uint16_t v)
{
    uint32_t old = HalPciRead32(d, off & ~3);
    unsigned shift = (off & 2) * 8;
    HalPciWrite32(d, off & ~3, (old & ~(0xFFFFu << shift)) | ((uint32_t)v << shift));
}

void HalPciWrite8(const HAL_PCI_DEVICE *d, uint16_t off, uint8_t v)
{
    uint32_t old = HalPciRead32(d, off & ~3);
    unsigned shift = (off & 3) * 8;
    HalPciWrite32(d, off & ~3, (old & ~(0xFFu << shift)) | ((uint32_t)v << shift));
}

bool HalPciUsingEcam(void)
{
    return g_ecam;
}

/* ---- Names -------------------------------------------------------------- */

const char *HalPciClassName(uint8_t cls, uint8_t sub, uint8_t pi)
{
    switch (cls) {
    case 0x00: return sub == 0x01 ? "VGA-compatible device" : "Unclassified device";
    case 0x01:
        switch (sub) {
        case 0x00: return "SCSI controller";
        case 0x01: return "IDE controller";
        case 0x02: return "Floppy controller";
        case 0x04: return "RAID controller";
        case 0x05: return "ATA controller";
        case 0x06: return pi == 0x01 ? "SATA controller (AHCI)" : "SATA controller";
        case 0x07: return "SAS controller";
        case 0x08: return pi == 0x02 ? "NVMe controller" : "Non-volatile memory controller";
        default:   return "Storage controller";
        }
    case 0x02:
        switch (sub) {
        case 0x00: return "Ethernet controller";
        case 0x80: return "Network controller";
        default:   return "Network controller";
        }
    case 0x03:
        switch (sub) {
        case 0x00: return "VGA controller";
        case 0x02: return "3D controller";
        default:   return "Display controller";
        }
    case 0x04:
        switch (sub) {
        case 0x01: return "Audio device";
        case 0x03: return "HD Audio controller";
        default:   return "Multimedia controller";
        }
    case 0x05: return "Memory controller";
    case 0x06:
        switch (sub) {
        case 0x00: return "Host bridge";
        case 0x01: return "ISA bridge";
        case 0x04: return "PCI bridge";
        case 0x80: return "Bridge";
        default:   return "Bridge";
        }
    case 0x07: return sub == 0x00 ? "Serial controller" : "Communication controller";
    case 0x08:
        switch (sub) {
        case 0x00: return "Interrupt controller";
        case 0x05: return "SD host controller";
        default:   return "System peripheral";
        }
    case 0x09: return "Input device controller";
    case 0x0C:
        switch (sub) {
        case 0x00: return "FireWire controller";
        case 0x03:
            switch (pi) {
            case 0x00: return "USB 1.1 controller (UHCI)";
            case 0x10: return "USB 1.1 controller (OHCI)";
            case 0x20: return "USB 2.0 controller (EHCI)";
            case 0x30: return "USB 3.x controller (xHCI)";
            case 0x40: return "USB4 controller";
            case 0xFE: return "USB device";
            default:   return "USB controller";
            }
        case 0x05: return "SMBus controller";
        default:   return "Serial bus controller";
        }
    case 0x0D: return "Wireless controller";
    case 0x10: return "Encryption controller";
    case 0x11: return "Signal processing controller";
    case 0x12: return "Processing accelerator";
    default:   return "Device";
    }
}

const char *HalPciVendorName(uint16_t v)
{
    switch (v) {
    case 0x8086: case 0x8087: return "Intel";
    case 0x1022: return "AMD";
    case 0x1002: return "AMD/ATI";
    case 0x10DE: return "NVIDIA";
    case 0x10EC: return "Realtek";
    case 0x1106: return "VIA";
    case 0x1B36: return "Red Hat (QEMU)";
    case 0x1AF4: return "Red Hat (virtio)";
    case 0x1234: return "QEMU/Bochs";
    case 0x15AD: return "VMware";
    case 0x80EE: return "VirtualBox";
    case 0x1039: return "SiS";
    case 0x104C: return "Texas Instruments";
    case 0x1033: return "NEC";
    case 0x1912: return "Renesas";
    case 0x1B21: return "ASMedia";
    case 0x14E4: return "Broadcom";
    case 0x168C: return "Qualcomm Atheros";
    case 0x144D: return "Samsung";
    case 0x1344: return "Micron";
    case 0x15B7: return "SanDisk";
    case 0x1987: return "Phison";
    case 0x1179: return "Toshiba";
    case 0x197B: return "JMicron";
    case 0x1283: return "ITE";
    case 0x1217: return "O2 Micro";
    case 0x1180: return "Ricoh";
    default:     return NULL;
    }
}

/* ---- Enumeration -------------------------------------------------------- */

static void size_bars(HAL_PCI_DEVICE *d, unsigned count)
{
    uint16_t cmd = HalPciRead16(d, 0x04);
    HalPciWrite16(d, 0x04, cmd & ~0x3);            /* stop decoding while sizing */
    for (unsigned i = 0; i < count; i++) {
        uint16_t off = 0x10 + i * 4;
        uint32_t orig = HalPciRead32(d, off);
        HalPciWrite32(d, off, 0xFFFFFFFF);
        uint32_t mask = HalPciRead32(d, off);
        HalPciWrite32(d, off, orig);
        if (mask == 0 || mask == 0xFFFFFFFF)
            continue;
        if (orig & 1) {                             /* I/O space */
            d->Bars[i].Io = true;
            d->Bars[i].Base = orig & ~3u;
            d->Bars[i].Size = (~(mask & ~3u) + 1) & 0xFFFF;
            continue;
        }
        unsigned type = (orig >> 1) & 3;
        d->Bars[i].Prefetchable = (orig >> 3) & 1;
        uint64_t base = orig & ~0xFull;
        uint64_t size_mask = mask & ~0xFull;
        if (type == 2 && i + 1 < count) {          /* 64-bit BAR uses two slots */
            uint16_t off2 = off + 4;
            uint32_t orig_hi = HalPciRead32(d, off2);
            HalPciWrite32(d, off2, 0xFFFFFFFF);
            uint32_t mask_hi = HalPciRead32(d, off2);
            HalPciWrite32(d, off2, orig_hi);
            base |= (uint64_t)orig_hi << 32;
            size_mask |= (uint64_t)mask_hi << 32;
            d->Bars[i].Is64 = true;
            d->Bars[i].Base = base;
            d->Bars[i].Size = ~size_mask + 1;
            i++;
        } else {
            d->Bars[i].Base = base;
            d->Bars[i].Size = (uint32_t)(~(uint32_t)size_mask + 1);
        }
    }
    HalPciWrite16(d, 0x04, cmd);
}

static void find_caps(HAL_PCI_DEVICE *d)
{
    if (!(HalPciRead16(d, 0x06) & (1u << 4)))      /* capability list present? */
        return;
    uint8_t ptr = HalPciRead8(d, (d->HeaderType & 0x7F) == 2 ? 0x14 : 0x34) & 0xFC;
    for (int guard = 0; ptr && guard < 48; guard++) {
        uint8_t id = HalPciRead8(d, ptr);
        if (id == 0x05) d->MsiCap = ptr;
        else if (id == 0x11) d->MsixCap = ptr;
        else if (id == 0x10) d->PcieCap = ptr;
        ptr = HalPciRead8(d, ptr + 1) & 0xFC;
    }
}

static void scan_bus(uint16_t seg, uint8_t bus);

static void add_function(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t fn)
{
    uint32_t id = cfg_read32(seg, bus, dev, fn, 0x00);
    if ((id & 0xFFFF) == 0xFFFF || g_count >= HAL_MAX_PCI_DEVICES)
        return;
    HAL_PCI_DEVICE *d = &g_devices[g_count++];
    memset(d, 0, sizeof(*d));
    d->Segment = seg;
    d->Bus = bus;
    d->Device = dev;
    d->Function = fn;
    d->VendorId = id & 0xFFFF;
    d->DeviceId = id >> 16;
    uint32_t cls = HalPciRead32(d, 0x08);
    d->Revision = cls & 0xFF;
    d->ProgIf = (cls >> 8) & 0xFF;
    d->SubClass = (cls >> 16) & 0xFF;
    d->ClassCode = cls >> 24;
    d->HeaderType = HalPciRead8(d, 0x0E);
    d->InterruptLine = HalPciRead8(d, 0x3C);
    d->InterruptPin = HalPciRead8(d, 0x3D);

    switch (d->HeaderType & 0x7F) {
    case 0:
        size_bars(d, 6);
        d->SubsystemVendorId = HalPciRead16(d, 0x2C);
        d->SubsystemId = HalPciRead16(d, 0x2E);
        break;
    case 1:
        size_bars(d, 2);
        break;
    }
    find_caps(d);

    if ((d->HeaderType & 0x7F) == 1) {              /* PCI-to-PCI bridge: follow it */
        uint8_t secondary = HalPciRead8(d, 0x19);
        if (secondary && secondary != bus)
            scan_bus(seg, secondary);
    }
}

static void scan_bus(uint16_t seg, uint8_t bus)
{
    if (g_scanned[bus / 8] & (1u << (bus % 8)))
        return;
    g_scanned[bus / 8] |= (uint8_t)(1u << (bus % 8));
    for (uint8_t dev = 0; dev < 32; dev++) {
        uint32_t id = cfg_read32(seg, bus, dev, 0, 0x00);
        if ((id & 0xFFFF) == 0xFFFF)
            continue;
        uint8_t header = (uint8_t)(cfg_read32(seg, bus, dev, 0, 0x0C) >> 16);
        unsigned fns = (header & 0x80) ? 8 : 1;
        for (uint8_t fn = 0; fn < fns; fn++)
            add_function(seg, bus, dev, fn);
    }
}

void HalInitPci(void)
{
    g_ecam = HalAcpi.EcamCount > 0;
    if (g_ecam) {
        for (unsigned i = 0; i < HalAcpi.EcamCount; i++)
            DbgPrint("PCI: ECAM segment %u buses %02x-%02x at 0x%llx", HalAcpi.Ecam[i].Segment,
                     HalAcpi.Ecam[i].StartBus, HalAcpi.Ecam[i].EndBus,
                     (unsigned long long)HalAcpi.Ecam[i].Base);
    } else {
        DbgPrint("PCI: using legacy configuration ports 0xCF8/0xCFC");
    }

    /* A multi-function host bridge at 00:00.0 means several root buses. */
    uint8_t header = (uint8_t)(cfg_read32(0, 0, 0, 0, 0x0C) >> 16);
    if (header & 0x80) {
        for (uint8_t fn = 0; fn < 8; fn++)
            if ((cfg_read32(0, 0, 0, fn, 0) & 0xFFFF) != 0xFFFF)
                scan_bus(0, fn);
    } else {
        scan_bus(0, 0);
    }

    for (unsigned i = 0; i < g_count; i++) {
        HAL_PCI_DEVICE *d = &g_devices[i];
        const char *vendor = HalPciVendorName(d->VendorId);
        char irq[16] = "";
        if (d->InterruptPin && d->InterruptLine && d->InterruptLine != 0xFF)
            snprintf(irq, sizeof(irq), ", IRQ %u", d->InterruptLine);
        DbgPrint("PCI: %02x:%02x.%u %04x:%04x %s%s%s%s%s%s", d->Bus, d->Device, d->Function,
                 d->VendorId, d->DeviceId, vendor ? vendor : "", vendor ? " " : "",
                 HalPciClassName(d->ClassCode, d->SubClass, d->ProgIf), irq,
                 d->MsixCap ? ", MSI-X" : (d->MsiCap ? ", MSI" : ""), d->PcieCap ? ", PCIe" : "");
    }
    DbgPrint("PCI: %u device%s found", g_count, g_count == 1 ? "" : "s");
}

unsigned HalPciDeviceCount(void)
{
    return g_count;
}

HAL_PCI_DEVICE *HalPciGetDevice(unsigned i)
{
    return i < g_count ? &g_devices[i] : NULL;
}

HAL_PCI_DEVICE *HalPciFindClass(uint8_t cls, uint8_t sub, uint8_t pi, unsigned n)
{
    for (unsigned i = 0; i < g_count; i++) {
        HAL_PCI_DEVICE *d = &g_devices[i];
        if (d->ClassCode == cls && d->SubClass == sub && (pi == 0xFF || d->ProgIf == pi) && n-- == 0)
            return d;
    }
    return NULL;
}

/* ---- Bus mastering and MSI ------------------------------------------------ */

void HalPciEnableBusMaster(const HAL_PCI_DEVICE *d)
{
    uint16_t cmd = HalPciRead16(d, 0x04);
    HalPciWrite16(d, 0x04, cmd | 0x0006);      /* memory space + bus master */
}

/* Route the device's MSI to 'vector' on this CPU. Returns false if the device
   has no MSI capability. Legacy INTx is turned off. */
bool HalPciEnableMsi(const HAL_PCI_DEVICE *d, uint8_t vector)
{
    if (!d->MsiCap)
        return false;
    uint8_t c = d->MsiCap;
    uint16_t ctrl = HalPciRead16(d, c + 2);
    bool is64 = ctrl & (1u << 7);
    uint32_t addr = 0xFEE00000u | (HalGetCurrentApicId() << 12);
    HalPciWrite32(d, c + 4, addr);
    if (is64) {
        HalPciWrite32(d, c + 8, 0);
        HalPciWrite16(d, c + 12, vector);
    } else {
        HalPciWrite16(d, c + 8, vector);
    }
    ctrl &= ~(uint16_t)(7u << 4);               /* one message */
    HalPciWrite16(d, c + 2, ctrl | 1);          /* enable */
    uint16_t cmd = HalPciRead16(d, 0x04);
    HalPciWrite16(d, 0x04, cmd | (1u << 10));   /* INTx off */
    return true;
}
