/*
 * ATA (IDE) port driver (\Driver\atapi).
 *
 * Finds IDE controllers on PCI (legacy or native mode) and the standard ISA
 * ports, identifies the master and slave drive on each channel, and gives the
 * disk class a read/write interface with LBA28 or LBA48 addressing.
 *
 * Transfers use bus-master DMA (PCI IDE controllers with a BMIDE register
 * block, through a bounce buffer below 4 GiB) when the drive supports it,
 * and PIO otherwise or after a DMA error. Interrupts are disabled on the
 * channel (nIEN) and completion is detected by polling.
 *
 * ATAPI devices (CD/DVD drives) are recognised and reported but not driven.
 */
#include "io.h"
#include "hal.h"
#include "mm.h"
#include "rtl.h"
#include "console.h"

#define REG_DATA       0
#define REG_ERROR      1
#define REG_COUNT      2
#define REG_LBA0       3
#define REG_LBA1       4
#define REG_LBA2       5
#define REG_DRIVE      6
#define REG_STATUS     7
#define REG_COMMAND    7

#define ST_ERR   0x01
#define ST_DRQ   0x08
#define ST_DF    0x20
#define ST_DRDY  0x40
#define ST_BSY   0x80

#define CMD_READ_PIO       0x20
#define CMD_READ_PIO_EXT   0x24
#define CMD_WRITE_PIO      0x30
#define CMD_WRITE_PIO_EXT  0x34
#define CMD_FLUSH          0xE7
#define CMD_FLUSH_EXT      0xEA
#define CMD_IDENTIFY       0xEC
#define CMD_READ_DMA       0xC8
#define CMD_READ_DMA_EXT   0x25
#define CMD_WRITE_DMA      0xCA
#define CMD_WRITE_DMA_EXT  0x35

/* Bus-master IDE registers (offset from the channel's BMIDE base) */
#define BM_CMD     0
#define BM_STATUS  2
#define BM_PRDT    4
#define BM_START   0x01
#define BM_TO_MEM  0x08
#define BM_ACTIVE  0x01
#define BM_ERROR   0x02
#define BM_IRQ     0x04
#define DMA_BYTES  (128 * 1024)

#define MAX_CHANNELS 8
#define TIMEOUT_MS   10000

typedef struct {
    uint16_t Base;          /* command block */
    uint16_t Ctrl;          /* control block (alternate status / device control) */
    KMUTEX Lock;
    uint8_t Selected;       /* last drive selected (0xFF = unknown) */
    bool Pio32;             /* PCI controllers accept 32-bit data port access */
    uint16_t Bm;            /* bus-master register block, 0 if none */
    uint32_t *Prdt;         /* physical region table (2 entries) */
    uint64_t PrdtPhys;
    uint8_t *DmaBuf;        /* 128 KiB bounce buffer below 4 GiB, 64 KiB aligned */
    uint64_t DmaPhys;
    const char *Name;
} CHANNEL;

typedef struct {
    CHANNEL *Channel;
    uint8_t Slave;
    bool Lba48;
    bool Dma;
    uint64_t Sectors;
    char Model[41];
    char Serial[21];
    char Bus[40];
} ATA_DRIVE;

static CHANNEL g_channels[MAX_CHANNELS];
static unsigned g_channel_count;

static void delay400(CHANNEL *c)
{
    for (int i = 0; i < 4; i++)
        (void)inb(c->Ctrl);
}

static NTSTATUS wait_not_busy(CHANNEL *c, uint8_t *status)
{
    uint64_t end = KeQueryUptimeNs() + (uint64_t)TIMEOUT_MS * 1000000;
    uint64_t spins = 0;
    for (;;) {
        uint8_t s = inb(c->Ctrl);
        if (!(s & ST_BSY)) {
            if (status)
                *status = s;
            return STATUS_SUCCESS;
        }
        if (KeQueryUptimeNs() > end)
            return STATUS_IO_TIMEOUT;
        if (++spins > 1000)
            KeYieldExecution();
        cpu_pause();
    }
}

static NTSTATUS wait_drq(CHANNEL *c)
{
    uint8_t s;
    NTSTATUS st = wait_not_busy(c, &s);
    if (!NT_SUCCESS(st))
        return st;
    if (s & (ST_ERR | ST_DF))
        return STATUS_IO_DEVICE_ERROR;
    if (!(s & ST_DRQ))
        return STATUS_IO_DEVICE_ERROR;
    return STATUS_SUCCESS;
}

static void select_drive(CHANNEL *c, uint8_t value)
{
    outb(c->Base + REG_DRIVE, value);
    delay400(c);
    c->Selected = value & 0x10;
}

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

/* ---- Transfers ------------------------------------------------------------- */

static NTSTATUS setup_command(ATA_DRIVE *d, uint64_t lba, uint32_t count, uint8_t cmd28, uint8_t cmd48)
{
    CHANNEL *c = d->Channel;
    NTSTATUS st = wait_not_busy(c, NULL);
    if (!NT_SUCCESS(st))
        return st;
    bool use48 = d->Lba48 && (lba + count > (1ull << 28) || count > 256);
    if (use48) {
        select_drive(c, (uint8_t)(0x40 | (d->Slave << 4)));
        outb(c->Base + REG_COUNT, (uint8_t)(count >> 8));
        outb(c->Base + REG_LBA0, (uint8_t)(lba >> 24));
        outb(c->Base + REG_LBA1, (uint8_t)(lba >> 32));
        outb(c->Base + REG_LBA2, (uint8_t)(lba >> 40));
        outb(c->Base + REG_COUNT, (uint8_t)count);
        outb(c->Base + REG_LBA0, (uint8_t)lba);
        outb(c->Base + REG_LBA1, (uint8_t)(lba >> 8));
        outb(c->Base + REG_LBA2, (uint8_t)(lba >> 16));
        outb(c->Base + REG_COMMAND, cmd48);
    } else {
        select_drive(c, (uint8_t)(0xE0 | (d->Slave << 4) | ((lba >> 24) & 0x0F)));
        outb(c->Base + REG_COUNT, (uint8_t)count);     /* 0 means 256 */
        outb(c->Base + REG_LBA0, (uint8_t)lba);
        outb(c->Base + REG_LBA1, (uint8_t)(lba >> 8));
        outb(c->Base + REG_LBA2, (uint8_t)(lba >> 16));
        outb(c->Base + REG_COMMAND, cmd28);
    }
    return STATUS_SUCCESS;
}

/* One DMA command of up to 256 sectors through the bounce buffer. */
static NTSTATUS dma_transfer(ATA_DRIVE *d, bool write, uint64_t lba, uint32_t count, void *buffer)
{
    CHANNEL *c = d->Channel;
    uint32_t bytes = count * 512;
    if (write)
        memcpy(c->DmaBuf, buffer, bytes);
    /* Regions may not cross 64 KiB: the buffer is 64 KiB aligned, so use two. */
    uint32_t first = MIN(bytes, 65536u), second = bytes - first;
    c->Prdt[0] = (uint32_t)c->DmaPhys;
    c->Prdt[1] = (first & 0xFFFF) | (second ? 0 : 0x80000000u);   /* 0 = 64 KiB */
    c->Prdt[2] = (uint32_t)(c->DmaPhys + 65536);
    c->Prdt[3] = (second & 0xFFFF) | 0x80000000u;
    outb(c->Bm + BM_CMD, 0);
    outl(c->Bm + BM_PRDT, (uint32_t)c->PrdtPhys);
    outb(c->Bm + BM_STATUS, inb(c->Bm + BM_STATUS) | BM_ERROR | BM_IRQ);   /* clear */
    outb(c->Bm + BM_CMD, write ? 0 : BM_TO_MEM);

    NTSTATUS st = setup_command(d, lba, count, write ? CMD_WRITE_DMA : CMD_READ_DMA,
                                write ? CMD_WRITE_DMA_EXT : CMD_READ_DMA_EXT);
    if (!NT_SUCCESS(st))
        return st;
    outb(c->Bm + BM_CMD, (uint8_t)((write ? 0 : BM_TO_MEM) | BM_START));
    delay400(c);
    uint8_t status;
    st = wait_not_busy(c, &status);
    uint64_t end = KeQueryUptimeNs() + 1000ull * 1000000;
    while (NT_SUCCESS(st) && (inb(c->Bm + BM_STATUS) & BM_ACTIVE) && KeQueryUptimeNs() < end)
        cpu_pause();
    uint8_t bm = inb(c->Bm + BM_STATUS);
    outb(c->Bm + BM_CMD, 0);
    outb(c->Bm + BM_STATUS, bm | BM_ERROR | BM_IRQ);
    if (NT_SUCCESS(st) && ((status & (ST_ERR | ST_DF)) || (bm & BM_ERROR) || (bm & BM_ACTIVE)))
        st = STATUS_IO_DEVICE_ERROR;
    if (NT_SUCCESS(st) && !write)
        memcpy(buffer, c->DmaBuf, bytes);
    return st;
}

static NTSTATUS try_dma(ATA_DRIVE *d, bool write, uint64_t lba, uint32_t count, void *buffer)
{
    CHANNEL *c = d->Channel;
    KeWaitForSingleObject(&c->Lock, KE_INFINITE);
    NTSTATUS st = dma_transfer(d, write, lba, count, buffer);
    KeReleaseMutex(&c->Lock);
    if (!NT_SUCCESS(st)) {
        DbgPrint("ATA: %s: DMA %s failed at LBA %llu (%s) - using PIO from now on", d->Bus,
                 write ? "write" : "read", (unsigned long long)lba, RtlStatusName(st));
        d->Dma = false;
    }
    return st;
}

static NTSTATUS ata_read(void *ctx, uint64_t lba, uint32_t count, void *buffer)
{
    if (((ATA_DRIVE *)ctx)->Dma && NT_SUCCESS(try_dma(ctx, false, lba, count, buffer)))
        return STATUS_SUCCESS;
    ATA_DRIVE *d = ctx;
    CHANNEL *c = d->Channel;
    KeWaitForSingleObject(&c->Lock, KE_INFINITE);
    NTSTATUS st = setup_command(d, lba, count, CMD_READ_PIO, CMD_READ_PIO_EXT);
    uint16_t *p = buffer;
    for (uint32_t s = 0; NT_SUCCESS(st) && s < count; s++) {
        delay400(c);
        st = wait_drq(c);
        if (!NT_SUCCESS(st))
            break;
        if (c->Pio32)
            __asm__ volatile("rep insl" : "+D"(p) : "d"(c->Base + REG_DATA), "c"(128) : "memory");
        else
            __asm__ volatile("rep insw" : "+D"(p) : "d"(c->Base + REG_DATA), "c"(256) : "memory");
    }
    KeReleaseMutex(&c->Lock);
    return st;
}

static NTSTATUS ata_write(void *ctx, uint64_t lba, uint32_t count, const void *buffer)
{
    if (((ATA_DRIVE *)ctx)->Dma && NT_SUCCESS(try_dma(ctx, true, lba, count, (void *)buffer)))
        return STATUS_SUCCESS;
    ATA_DRIVE *d = ctx;
    CHANNEL *c = d->Channel;
    KeWaitForSingleObject(&c->Lock, KE_INFINITE);
    NTSTATUS st = setup_command(d, lba, count, CMD_WRITE_PIO, CMD_WRITE_PIO_EXT);
    const uint16_t *p = buffer;
    for (uint32_t s = 0; NT_SUCCESS(st) && s < count; s++) {
        delay400(c);
        st = wait_drq(c);
        if (!NT_SUCCESS(st))
            break;
        if (c->Pio32) {
            __asm__ volatile("rep outsl" : "+S"(p) : "d"(c->Base + REG_DATA), "c"(128) : "memory");
        } else {
            for (int i = 0; i < 256; i++)
                outw(c->Base + REG_DATA, *p++);
        }
    }
    if (NT_SUCCESS(st)) {
        uint8_t status;
        delay400(c);
        st = wait_not_busy(c, &status);
        if (NT_SUCCESS(st) && (status & (ST_ERR | ST_DF)))
            st = STATUS_IO_DEVICE_ERROR;
    }
    KeReleaseMutex(&c->Lock);
    return st;
}

static NTSTATUS ata_flush(void *ctx)
{
    ATA_DRIVE *d = ctx;
    CHANNEL *c = d->Channel;
    KeWaitForSingleObject(&c->Lock, KE_INFINITE);
    NTSTATUS st = wait_not_busy(c, NULL);
    if (NT_SUCCESS(st)) {
        select_drive(c, (uint8_t)(0xE0 | (d->Slave << 4)));
        outb(c->Base + REG_COMMAND, d->Lba48 ? CMD_FLUSH_EXT : CMD_FLUSH);
        uint8_t status;
        delay400(c);
        st = wait_not_busy(c, &status);
        if (NT_SUCCESS(st) && (status & (ST_ERR | ST_DF)))
            st = STATUS_IO_DEVICE_ERROR;
    }
    KeReleaseMutex(&c->Lock);
    return st;
}

static const DISK_PORT_OPS g_ops = { ata_read, ata_write, ata_flush, NULL };

/* ---- Detection ------------------------------------------------------------- */

static void probe_drive(CHANNEL *c, uint8_t slave)
{
    select_drive(c, (uint8_t)(0xA0 | (slave << 4)));
    uint8_t s = inb(c->Base + REG_STATUS);
    if (s == 0xFF || s == 0x7F)
        return;                                 /* floating bus: nothing there */
    outb(c->Base + REG_COUNT, 0);
    outb(c->Base + REG_LBA0, 0);
    outb(c->Base + REG_LBA1, 0);
    outb(c->Base + REG_LBA2, 0);
    outb(c->Base + REG_COMMAND, CMD_IDENTIFY);
    delay400(c);
    if (inb(c->Base + REG_STATUS) == 0)
        return;                                 /* no drive */
    uint64_t end = KeQueryUptimeNs() + 2000ull * 1000000;
    while ((inb(c->Ctrl) & ST_BSY) && KeQueryUptimeNs() < end)
        cpu_pause();
    if (inb(c->Ctrl) & ST_BSY)
        return;
    uint8_t m = inb(c->Base + REG_LBA1), h = inb(c->Base + REG_LBA2);
    const char *pos = slave ? "slave" : "master";
    if ((m == 0x14 && h == 0xEB) || (m == 0x69 && h == 0x96)) {
        DbgPrint("ATA: %s %s: ATAPI device (CD/DVD) - not supported yet", c->Name, pos);
        return;
    }
    if (m || h)
        return;                                 /* not an ATA device */
    for (;;) {
        s = inb(c->Ctrl);
        if (s & ST_ERR)
            return;
        if (s & ST_DRQ)
            break;
        if (KeQueryUptimeNs() > end)
            return;
    }
    uint16_t id[256];
    uint16_t *p = id;
    __asm__ volatile("rep insw" : "+D"(p) : "d"(c->Base + REG_DATA), "c"(256) : "memory");

    ATA_DRIVE *d = ExAllocatePoolWithTag(NonPagedPoolZeroed, sizeof(ATA_DRIVE), 'Ata ');
    if (!d)
        return;
    d->Channel = c;
    d->Slave = slave;
    copy_ata_string(d->Model, id, 27, 20);
    copy_ata_string(d->Serial, id, 10, 10);
    d->Lba48 = (id[83] & (1u << 10)) != 0;
    uint64_t lba28 = (uint64_t)id[60] | ((uint64_t)id[61] << 16);
    uint64_t lba48 = (uint64_t)id[100] | ((uint64_t)id[101] << 16) | ((uint64_t)id[102] << 32) |
                     ((uint64_t)id[103] << 48);
    d->Sectors = d->Lba48 && lba48 ? lba48 : lba28;
    if (!(id[49] & (1u << 9)) || d->Sectors == 0) {
        DbgPrint("ATA: %s %s: \"%s\" has no LBA support - ignored", c->Name, pos, d->Model);
        ExFreePoolWithTag(d, 'Ata ');
        return;
    }
    d->Dma = c->Bm && (id[49] & (1u << 8));     /* controller and drive can do DMA */
    snprintf(d->Bus, sizeof(d->Bus), "IDE %s %s, %s, %s", c->Name, pos, d->Lba48 ? "LBA48" : "LBA28",
             d->Dma ? "DMA" : "PIO");

    DISK_PARAMS dp = { 0 };
    dp.Model = d->Model;
    dp.Serial = d->Serial;
    dp.Bus = d->Bus;
    dp.BytesPerSector = 512;
    dp.SectorCount = d->Sectors;
    dp.MaxTransferSectors = d->Lba48 || d->Dma ? 256 : 128;
    dp.Ops = &g_ops;
    dp.Context = d;
    DiskCreate(&dp, NULL);
}

static void add_channel(uint16_t base, uint16_t ctrl, const char *name, bool pci, uint16_t bm)
{
    for (unsigned i = 0; i < g_channel_count; i++)
        if (g_channels[i].Base == base)
            return;                             /* already known */
    if (g_channel_count >= MAX_CHANNELS)
        return;
    CHANNEL *c = &g_channels[g_channel_count];
    c->Base = base;
    c->Ctrl = ctrl;
    c->Name = name;
    c->Selected = 0xFF;
    c->Pio32 = pci;
    KeInitializeMutex(&c->Lock);
    if (bm) {
        uint64_t prdt = MmAllocatePhysicalPages(1, MM_BELOW_4G, 0);
        uint64_t buf = MmAllocatePhysicalPages(DMA_BYTES / PAGE_SIZE, MM_BELOW_4G, 65536);
        if (prdt && buf) {
            c->Bm = bm;
            c->Prdt = MmPhysToVirt(prdt);
            c->PrdtPhys = prdt;
            c->DmaBuf = MmPhysToVirt(buf);
            c->DmaPhys = buf;
        }
    }
    /* Is anything there at all? A floating bus reads 0xFF. */
    if (inb(ctrl) == 0xFF && inb(base + REG_STATUS) == 0xFF)
        return;
    g_channel_count++;
    outb(ctrl, 0x02);                           /* nIEN: no interrupts */
    outb(ctrl, 0x06);                           /* software reset */
    HalStallMs(1);
    outb(ctrl, 0x02);
    HalStallMs(2);
    uint64_t end = KeQueryUptimeNs() + 3000ull * 1000000;
    while ((inb(ctrl) & ST_BSY) && KeQueryUptimeNs() < end)
        cpu_pause();
    probe_drive(c, 0);
    probe_drive(c, 1);
}

static NTSTATUS atapi_entry(DRIVER_OBJECT *driver)
{
    UNUSED(driver);
    static const char *names[] = { "primary", "secondary", "tertiary", "quaternary" };
    unsigned n = 0;
    for (unsigned i = 0;; i++) {
        HAL_PCI_DEVICE *d = HalPciFindClass(0x01, 0x01, 0xFF, i);
        if (!d)
            break;
        HalPciWrite16(d, 0x04, HalPciRead16(d, 0x04) | 0x0005);   /* I/O space + bus master */
        for (int ch = 0; ch < 2; ch++) {
            bool native = d->ProgIf & (ch ? 0x04 : 0x01);
            uint16_t base, ctrl;
            if (native && d->Bars[ch * 2].Io && d->Bars[ch * 2 + 1].Io) {
                base = (uint16_t)d->Bars[ch * 2].Base;
                ctrl = (uint16_t)(d->Bars[ch * 2 + 1].Base + 2);
            } else {
                base = ch ? 0x170 : 0x1F0;
                ctrl = ch ? 0x376 : 0x3F6;
            }
            uint16_t bm = d->Bars[4].Io && d->Bars[4].Base ? (uint16_t)(d->Bars[4].Base + ch * 8) : 0;
            add_channel(base, ctrl, names[n < 4 ? n : 3], true, bm);
            n++;
        }
    }
    if (n == 0) {
        /* No PCI IDE controller: try the classic ISA ports. */
        add_channel(0x1F0, 0x3F6, "primary", false, 0);
        add_channel(0x170, 0x376, "secondary", false, 0);
    }
    return STATUS_SUCCESS;
}

void AtaInitialize(void)
{
    IoCreateDriver("atapi", atapi_entry);
}
