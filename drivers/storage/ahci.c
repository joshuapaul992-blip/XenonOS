/*
 * AHCI (SATA) port driver (\Driver\storahci).
 *
 * For each AHCI controller: take it over from the BIOS, enable AHCI mode,
 * and for every port with a SATA disk attached set up a command list,
 * receive area and command table, identify the drive and register it with
 * the disk class. Reads and writes use DMA (READ/WRITE DMA EXT) with a
 * physical region table built from the caller's buffer pages; a bounce buffer
 * is used for buffers the controller cannot reach. Completion is signalled by
 * MSI when the controller has it, otherwise the command register is polled.
 */
#include "io.h"
#include "hal.h"
#include "mm.h"
#include "rtl.h"
#include "console.h"

/* HBA registers */
#define HBA_CAP    0x00
#define HBA_GHC    0x04
#define HBA_IS     0x08
#define HBA_PI     0x0C
#define HBA_VS     0x10
#define HBA_CAP2   0x24
#define HBA_BOHC   0x28
#define GHC_AE     (1u << 31)
#define GHC_IE     (1u << 1)
#define CAP_S64A   (1u << 31)

/* Port registers (offset from 0x100 + port * 0x80) */
#define PX_CLB     0x00
#define PX_CLBU    0x04
#define PX_FB      0x08
#define PX_FBU     0x0C
#define PX_IS      0x10
#define PX_IE      0x14
#define PX_CMD     0x18
#define PX_TFD     0x20
#define PX_SIG     0x24
#define PX_SSTS    0x28
#define PX_SERR    0x30
#define PX_CI      0x38
#define CMD_ST     (1u << 0)
#define CMD_SUD    (1u << 1)
#define CMD_POD    (1u << 2)
#define CMD_FRE    (1u << 4)
#define CMD_FR     (1u << 14)
#define CMD_CR     (1u << 15)
#define IS_TFES    (1u << 30)
#define IS_ERRORS  (IS_TFES | (1u << 29) | (1u << 28) | (1u << 27))
#define TFD_BSY    0x80
#define TFD_DRQ    0x08
#define TFD_ERR    0x01

#define SIG_ATA    0x00000101
#define SIG_ATAPI  0xEB140101

#define MAX_PRDT       64
#define MAX_SECTORS    256              /* 128 KiB per command */
#define TIMEOUT_MS     10000

typedef struct AHCI_HBA AHCI_HBA;

typedef struct {
    AHCI_HBA *Hba;
    unsigned Index;
    volatile uint8_t *Regs;
    uint8_t *Mem;                       /* command list, FIS area, command table */
    uint64_t MemPhys;
    uint8_t *Bounce;
    uint64_t BouncePhys;
    KEVENT Done;
    volatile uint32_t LastIs;
    bool Lba48;
    uint64_t Sectors;
    char Model[41];
    char Serial[21];
    char Bus[40];
} AHCI_PORT;

struct AHCI_HBA {
    volatile uint8_t *Abar;
    HAL_PCI_DEVICE *Pci;
    bool S64;
    bool Msi;
    unsigned Number;
    AHCI_PORT *Ports[32];
    KDPC Dpc;
    volatile uint32_t PendingPorts;
};

static unsigned g_hba_count;

static inline uint32_t hr(AHCI_HBA *h, uint32_t r) { return *(volatile uint32_t *)(h->Abar + r); }
static inline void hw(AHCI_HBA *h, uint32_t r, uint32_t v) { *(volatile uint32_t *)(h->Abar + r) = v; }
static inline uint32_t pr(AHCI_PORT *p, uint32_t r) { return *(volatile uint32_t *)(p->Regs + r); }
static inline void pw(AHCI_PORT *p, uint32_t r, uint32_t v) { *(volatile uint32_t *)(p->Regs + r) = v; }

static bool wait_clear(AHCI_PORT *p, uint32_t reg, uint32_t mask, unsigned ms)
{
    uint64_t end = KeQueryUptimeNs() + (uint64_t)ms * 1000000;
    while (pr(p, reg) & mask) {
        if (KeQueryUptimeNs() > end)
            return false;
        cpu_pause();
    }
    return true;
}

static void port_stop(AHCI_PORT *p)
{
    pw(p, PX_CMD, pr(p, PX_CMD) & ~CMD_ST);
    wait_clear(p, PX_CMD, CMD_CR, 500);
    pw(p, PX_CMD, pr(p, PX_CMD) & ~CMD_FRE);
    wait_clear(p, PX_CMD, CMD_FR, 500);
}

static void port_start(AHCI_PORT *p)
{
    wait_clear(p, PX_CMD, CMD_CR, 500);
    pw(p, PX_CMD, pr(p, PX_CMD) | CMD_FRE | CMD_SUD | CMD_POD);
    pw(p, PX_CMD, pr(p, PX_CMD) | CMD_ST);
}

/* ---- Interrupts -------------------------------------------------------- */

static void hba_dpc(KDPC *dpc, void *ctx, void *a1, void *a2)
{
    UNUSED(dpc); UNUSED(a1); UNUSED(a2);
    AHCI_HBA *h = ctx;
    uint32_t pending = __atomic_exchange_n(&h->PendingPorts, 0, __ATOMIC_ACQ_REL);
    for (unsigned i = 0; i < 32; i++)
        if ((pending & (1u << i)) && h->Ports[i])
            KeSetEvent(&h->Ports[i]->Done);
}

static void hba_isr(KTRAP_FRAME *f, void *ctx)
{
    UNUSED(f);
    AHCI_HBA *h = ctx;
    uint32_t is = hr(h, HBA_IS);
    for (unsigned i = 0; i < 32; i++) {
        if (!(is & (1u << i)) || !h->Ports[i])
            continue;
        AHCI_PORT *p = h->Ports[i];
        uint32_t pis = pr(p, PX_IS);
        pw(p, PX_IS, pis);                     /* acknowledge */
        p->LastIs |= pis;
        __atomic_or_fetch(&h->PendingPorts, 1u << i, __ATOMIC_RELEASE);
    }
    hw(h, HBA_IS, is);
    KeInsertQueueDpc(&h->Dpc, NULL, NULL);
}

/* ---- Commands ----------------------------------------------------------- */

/* Layout of the per-port page: command list at 0 (1 KiB, only slot 0 used),
   received FIS at 0x400 (256 bytes), command table at 0x500. */
#define OFF_CL   0x000
#define OFF_FIS  0x400
#define OFF_CT   0x500

static NTSTATUS issue(AHCI_PORT *p, uint8_t command, uint64_t lba, uint32_t count, void *buffer,
                      uint32_t bytes, bool write)
{
    uint32_t *hdr = (uint32_t *)(p->Mem + OFF_CL);
    uint8_t *ct = p->Mem + OFF_CT;
    memset(ct, 0, 0x80 + MAX_PRDT * 16);

    /* Physical region descriptors: one per physically contiguous piece. */
    unsigned prdt = 0;
    uint8_t *va = buffer;
    uint32_t left = bytes;
    bool bounce = false;
    if (bytes && (((uintptr_t)buffer & 1) || bytes > MAX_SECTORS * 512))
        bounce = true;
    while (left && !bounce) {
        uint64_t pa = MmGetPhysicalAddress(va);
        uint32_t chunk = (uint32_t)MIN((uint64_t)left, PAGE_SIZE - ((uintptr_t)va & (PAGE_SIZE - 1)));
        if (!pa || (!p->Hba->S64 && pa + chunk > MM_BELOW_4G)) {
            bounce = true;
            break;
        }
        uint32_t *e = prdt ? (uint32_t *)(ct + 0x80 + (prdt - 1) * 16) : NULL;
        uint64_t prev_end = e ? ((uint64_t)e[0] | ((uint64_t)e[1] << 32)) + (e[3] & 0x3FFFFF) + 1 : 0;
        if (e && prev_end == pa && (e[3] & 0x3FFFFF) + chunk < (4u << 20)) {
            e[3] += chunk;                     /* extend the previous region */
        } else {
            if (prdt == MAX_PRDT) {
                bounce = true;
                break;
            }
            e = (uint32_t *)(ct + 0x80 + prdt * 16);
            e[0] = (uint32_t)pa;
            e[1] = (uint32_t)(pa >> 32);
            e[2] = 0;
            e[3] = chunk - 1;
            prdt++;
        }
        va += chunk;
        left -= chunk;
    }
    if (bounce) {
        memset(ct + 0x80, 0, MAX_PRDT * 16);
        if (write)
            memcpy(p->Bounce, buffer, bytes);
        uint32_t *e = (uint32_t *)(ct + 0x80);
        e[0] = (uint32_t)p->BouncePhys;
        e[1] = (uint32_t)(p->BouncePhys >> 32);
        e[3] = bytes - 1;
        prdt = 1;
    }

    /* Register FIS, host to device. */
    uint8_t *fis = ct;
    fis[0] = 0x27;
    fis[1] = 0x80;                              /* command, not control */
    fis[2] = command;
    fis[4] = (uint8_t)lba;
    fis[5] = (uint8_t)(lba >> 8);
    fis[6] = (uint8_t)(lba >> 16);
    fis[7] = command == 0xEC ? 0 : 0x40;        /* LBA mode */
    fis[8] = (uint8_t)(lba >> 24);
    fis[9] = (uint8_t)(lba >> 32);
    fis[10] = (uint8_t)(lba >> 40);
    fis[12] = (uint8_t)count;
    fis[13] = (uint8_t)(count >> 8);

    hdr[0] = 5 | (write ? (1u << 6) : 0) | ((uint32_t)prdt << 16);
    hdr[1] = 0;
    uint64_t ct_phys = p->MemPhys + OFF_CT;
    hdr[2] = (uint32_t)ct_phys;
    hdr[3] = (uint32_t)(ct_phys >> 32);

    if (!wait_clear(p, PX_TFD, TFD_BSY | TFD_DRQ, TIMEOUT_MS))
        return STATUS_DEVICE_NOT_READY;
    KeClearEvent(&p->Done);
    p->LastIs = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    pw(p, PX_CI, 1);

    uint64_t end = KeQueryUptimeNs() + (uint64_t)TIMEOUT_MS * 1000000;
    NTSTATUS st = STATUS_SUCCESS;
    for (;;) {
        uint32_t is = p->Hba->Msi ? p->LastIs : pr(p, PX_IS);
        if (is & IS_ERRORS) {
            st = STATUS_IO_DEVICE_ERROR;
            break;
        }
        if (!(pr(p, PX_CI) & 1))
            break;
        if (KeQueryUptimeNs() > end) {
            st = STATUS_IO_TIMEOUT;
            break;
        }
        if (p->Hba->Msi)
            KeWaitForSingleObject(&p->Done, -100 * 1000000ll);
        else
            cpu_pause();
    }
    if (!p->Hba->Msi)
        pw(p, PX_IS, pr(p, PX_IS));
    if (NT_SUCCESS(st) && (pr(p, PX_TFD) & TFD_ERR))
        st = STATUS_IO_DEVICE_ERROR;
    if (!NT_SUCCESS(st)) {
        /* Recover the port: stop, clear errors, start again. */
        DbgPrint("AHCI: %s: command 0x%02x at LBA %llu failed (TFD 0x%x, IS 0x%x): %s", p->Bus, command,
                 (unsigned long long)lba, pr(p, PX_TFD), (unsigned)p->LastIs, RtlStatusName(st));
        port_stop(p);
        pw(p, PX_SERR, 0xFFFFFFFF);
        pw(p, PX_IS, 0xFFFFFFFF);
        port_start(p);
        return st;
    }
    if (bounce && !write)
        memcpy(buffer, p->Bounce, bytes);
    return STATUS_SUCCESS;
}

static NTSTATUS ahci_read(void *ctx, uint64_t lba, uint32_t count, void *buffer)
{
    return issue(ctx, 0x25, lba, count, buffer, count * 512, false);     /* READ DMA EXT */
}

static NTSTATUS ahci_write(void *ctx, uint64_t lba, uint32_t count, const void *buffer)
{
    return issue(ctx, 0x35, lba, count, (void *)buffer, count * 512, true);   /* WRITE DMA EXT */
}

static NTSTATUS ahci_flush(void *ctx)
{
    return issue(ctx, 0xEA, 0, 0, NULL, 0, false);                       /* FLUSH CACHE EXT */
}

static const DISK_PORT_OPS g_ops = { ahci_read, ahci_write, ahci_flush, NULL };

/* ---- Set-up ----------------------------------------------------------------- */

static void copy_ata_string(char *out, const uint16_t *id, int first, int words)
{
    int n = 0;
    for (int i = 0; i < words; i++) {
        out[n++] = (char)(id[first + i] >> 8);
        out[n++] = (char)(id[first + i] & 0xFF);
    }
    out[n] = 0;
    while (n > 0 && out[n - 1] == ' ')
        out[--n] = 0;
    int lead = 0;
    while (out[lead] == ' ')
        lead++;
    if (lead)
        memmove(out, out + lead, (size_t)(n - lead + 1));
}

static void init_port(AHCI_HBA *h, unsigned i)
{
    volatile uint8_t *regs = h->Abar + 0x100 + i * 0x80;
    uint32_t ssts = *(volatile uint32_t *)(regs + PX_SSTS);
    if ((ssts & 0xF) != 3 || ((ssts >> 8) & 0xF) != 1)
        return;                                 /* no device, or not active */
    uint32_t sig = *(volatile uint32_t *)(regs + PX_SIG);
    if (sig == SIG_ATAPI) {
        DbgPrint("AHCI: controller %u port %u: ATAPI device (CD/DVD) - not supported yet", h->Number, i);
        return;
    }
    if (sig != SIG_ATA) {
        DbgPrint("AHCI: controller %u port %u: device signature 0x%08x not supported", h->Number, i, sig);
        return;
    }

    AHCI_PORT *p = ExAllocatePoolWithTag(NonPagedPoolZeroed, sizeof(AHCI_PORT), 'Ahci');
    uint64_t limit = h->S64 ? MM_ANY_ADDRESS : MM_BELOW_4G;
    uint64_t mem = MmAllocatePhysicalPages(1, limit, 0);
    uint64_t bounce = MmAllocatePhysicalPages(MAX_SECTORS * 512 / PAGE_SIZE, limit, 0);
    if (!p || !mem || !bounce)
        return;
    p->Hba = h;
    p->Index = i;
    p->Regs = regs;
    p->Mem = MmPhysToVirt(mem);
    p->MemPhys = mem;
    p->Bounce = MmPhysToVirt(bounce);
    p->BouncePhys = bounce;
    KeInitializeEvent(&p->Done, SynchronizationEvent, false);

    port_stop(p);
    pw(p, PX_CLB, (uint32_t)(mem + OFF_CL));
    pw(p, PX_CLBU, (uint32_t)((mem + OFF_CL) >> 32));
    pw(p, PX_FB, (uint32_t)(mem + OFF_FIS));
    pw(p, PX_FBU, (uint32_t)((mem + OFF_FIS) >> 32));
    pw(p, PX_SERR, 0xFFFFFFFF);
    pw(p, PX_IS, 0xFFFFFFFF);
    h->Ports[i] = p;
    pw(p, PX_IE, h->Msi ? (0x1u | 0x2u | IS_ERRORS) : 0);     /* D2H FIS, PIO setup, errors */
    port_start(p);

    uint16_t *id = ExAllocatePoolWithTag(NonPagedPool, 512, 'Ahci');
    if (!id || !NT_SUCCESS(issue(p, 0xEC, 0, 0, id, 512, false))) {
        DbgPrint("AHCI: controller %u port %u: IDENTIFY failed", h->Number, i);
        h->Ports[i] = NULL;
        if (id)
            ExFreePoolWithTag(id, 'Ahci');
        return;
    }
    copy_ata_string(p->Model, id, 27, 20);
    copy_ata_string(p->Serial, id, 10, 10);
    p->Lba48 = (id[83] & (1u << 10)) != 0;
    p->Sectors = p->Lba48 ? ((uint64_t)id[100] | ((uint64_t)id[101] << 16) | ((uint64_t)id[102] << 32) |
                             ((uint64_t)id[103] << 48))
                          : ((uint64_t)id[60] | ((uint64_t)id[61] << 16));
    ExFreePoolWithTag(id, 'Ahci');
    snprintf(p->Bus, sizeof(p->Bus), "AHCI controller %u port %u, SATA", h->Number, i);

    DISK_PARAMS dp = { 0 };
    dp.Model = p->Model;
    dp.Serial = p->Serial;
    dp.Bus = p->Bus;
    dp.BytesPerSector = 512;
    dp.SectorCount = p->Sectors;
    dp.MaxTransferSectors = MAX_SECTORS;
    dp.Ops = &g_ops;
    dp.Context = p;
    DiskCreate(&dp, NULL);
}

static void init_hba(HAL_PCI_DEVICE *d)
{
    if (!d->Bars[5].Base || d->Bars[5].Io) {
        DbgPrint("AHCI: %02x:%02x.%u has no ABAR - ignored", d->Bus, d->Device, d->Function);
        return;
    }
    AHCI_HBA *h = ExAllocatePoolWithTag(NonPagedPoolZeroed, sizeof(AHCI_HBA), 'Ahci');
    if (!h)
        return;
    h->Pci = d;
    h->Number = g_hba_count++;
    size_t size = d->Bars[5].Size ? d->Bars[5].Size : 0x1100;
    h->Abar = MmMapIoSpace(d->Bars[5].Base, size, MmNonCached);
    if (!h->Abar)
        return;
    HalPciEnableBusMaster(d);

    /* BIOS/OS handoff. */
    if (hr(h, HBA_CAP2) & 1) {
        hw(h, HBA_BOHC, hr(h, HBA_BOHC) | 2);   /* OS ownership request */
        uint64_t end = KeQueryUptimeNs() + 25ull * 1000000;
        while ((hr(h, HBA_BOHC) & 1) && KeQueryUptimeNs() < end)
            cpu_pause();
        if (hr(h, HBA_BOHC) & 0x10)              /* BIOS busy: allow up to 2 s */
            KeSleepMs(2000);
    }
    hw(h, HBA_GHC, hr(h, HBA_GHC) | GHC_AE);
    uint32_t cap = hr(h, HBA_CAP);
    h->S64 = (cap & CAP_S64A) != 0;

    KeInitializeDpc(&h->Dpc, hba_dpc, h);
    uint8_t vec = KeAllocateVectorAtIrql(6);
    if (vec && d->MsiCap) {
        KeConnectInterrupt(vec, hba_isr, h);
        h->Msi = HalPciEnableMsi(d, vec);
    }
    hw(h, HBA_IS, 0xFFFFFFFF);
    if (h->Msi)
        hw(h, HBA_GHC, hr(h, HBA_GHC) | GHC_IE);

    uint32_t vs = hr(h, HBA_VS), pi = hr(h, HBA_PI);
    DbgPrint("AHCI: controller %u at %02x:%02x.%u: AHCI %u.%u, %u ports (map 0x%x), %s DMA, %s",
             h->Number, d->Bus, d->Device, d->Function, vs >> 16, (vs >> 8) & 0xFF, (cap & 0x1F) + 1, pi,
             h->S64 ? "64-bit" : "32-bit", h->Msi ? "MSI interrupts" : "polling");
    for (unsigned i = 0; i < 32; i++)
        if (pi & (1u << i))
            init_port(h, i);
}

static NTSTATUS storahci_entry(DRIVER_OBJECT *driver)
{
    UNUSED(driver);
    for (unsigned i = 0;; i++) {
        HAL_PCI_DEVICE *d = HalPciFindClass(0x01, 0x06, 0x01, i);
        if (!d)
            break;
        init_hba(d);
    }
    return STATUS_SUCCESS;
}

void AhciInitialize(void)
{
    IoCreateDriver("storahci", storahci_entry);
}
