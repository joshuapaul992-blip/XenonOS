/*
 * Floppy disk controller driver (\Driver\fdc), for 82077AA-compatible
 * controllers at the standard ports 0x3F0-0x3F7.
 *
 * Drives come from the CMOS (drive A: and B: types). The controller signals
 * the end of seeks, recalibrations and transfers on IRQ 6; data moves by ISA
 * DMA channel 2 through a buffer below 16 MiB. Transfers never cross a track
 * (one head of one cylinder).
 *
 * When a medium is first used, or after the disk-change line says it was
 * swapped, the driver finds the data rate that works (500 kbit/s for
 * 1.44 MB, 1 Mbit/s for 2.88 MB, 250 kbit/s for 720 KB) and then takes the
 * geometry from the FAT boot sector when there is one.
 *
 * The motor is switched on when needed and off after 3 seconds without use.
 */
#include "io.h"
#include "hal.h"
#include "mm.h"
#include "ps.h"
#include "rtl.h"
#include "console.h"

#define FDC_DOR   0x3F2
#define FDC_MSR   0x3F4
#define FDC_FIFO  0x3F5
#define FDC_DIR   0x3F7          /* read */
#define FDC_CCR   0x3F7          /* write */

#define MSR_RQM   0x80
#define MSR_DIO   0x40
#define DIR_CHANGED 0x80

#define CMD_SPECIFY     0x03
#define CMD_WRITE       0xC5      /* MT + MFM + WRITE DATA */
#define CMD_READ        0xE6      /* MT + MFM + SK + READ DATA */
#define CMD_RECALIBRATE 0x07
#define CMD_SENSE_INT   0x08
#define CMD_SEEK        0x0F
#define CMD_READ_ID     0x4A      /* MFM + READ ID */
#define CMD_VERSION     0x10
#define CMD_CONFIGURE   0x13

#define DMA_BUFFER_SIZE (36 * 512)   /* one track of a 2.88 MB disk */

typedef struct {
    unsigned Drive;              /* 0 = A:, 1 = B: */
    uint8_t CmosType;
    char Model[40];
    char Bus[40];
    DEVICE_OBJECT *Disk;
    bool Known;                  /* geometry valid for the current medium */
    bool Present;
    uint8_t Rate;                /* CCR value */
    uint8_t Gap;
    uint8_t Cylinders, Heads, Spt;
    int CurrentCyl;              /* -1 = unknown, recalibrate first */
    bool WriteProtected;
} FLOPPY;

static FLOPPY g_drives[2];
static KMUTEX g_lock;
static KEVENT g_irq_event;
static KDPC g_dpc;
static uint8_t *g_dma;
static uint64_t g_dma_phys;
static uint8_t g_dor = 0x0C;         /* DMA/IRQ on, not in reset */
static uint64_t g_last_use;
static bool g_enhanced;

/* ---- Low level ----------------------------------------------------------- */

static void irq_dpc(KDPC *d, void *c, void *a1, void *a2)
{
    UNUSED(d); UNUSED(c); UNUSED(a1); UNUSED(a2);
    KeSetEvent(&g_irq_event);
}

static void irq_isr(KTRAP_FRAME *f, void *ctx)
{
    UNUSED(f); UNUSED(ctx);
    KeInsertQueueDpc(&g_dpc, NULL, NULL);
}

static bool wait_irq(unsigned ms)
{
    return KeWaitForSingleObject(&g_irq_event, -(int64_t)ms * 1000000) == STATUS_WAIT_0;
}

static bool fdc_write(uint8_t b)
{
    uint64_t end = KeQueryUptimeNs() + 500ull * 1000000;
    while (KeQueryUptimeNs() < end) {
        uint8_t msr = inb(FDC_MSR);
        if ((msr & (MSR_RQM | MSR_DIO)) == MSR_RQM) {
            outb(FDC_FIFO, b);
            return true;
        }
        cpu_pause();
    }
    return false;
}

static int fdc_read(void)
{
    uint64_t end = KeQueryUptimeNs() + 500ull * 1000000;
    while (KeQueryUptimeNs() < end) {
        uint8_t msr = inb(FDC_MSR);
        if ((msr & (MSR_RQM | MSR_DIO)) == (MSR_RQM | MSR_DIO))
            return inb(FDC_FIFO);
        cpu_pause();
    }
    return -1;
}

static bool sense_interrupt(uint8_t *st0, uint8_t *cyl)
{
    if (!fdc_write(CMD_SENSE_INT))
        return false;
    int a = fdc_read(), b = fdc_read();
    if (a < 0 || b < 0)
        return false;
    if (st0) *st0 = (uint8_t)a;
    if (cyl) *cyl = (uint8_t)b;
    return true;
}

static void configure_controller(void)
{
    if (g_enhanced) {
        fdc_write(CMD_CONFIGURE);
        fdc_write(0x00);
        fdc_write(0x57);          /* implied seek on, FIFO on, polling off, threshold 8 */
        fdc_write(0x00);
    }
    fdc_write(CMD_SPECIFY);
    fdc_write(0xDF);              /* step rate 3 ms, head unload 240 ms */
    fdc_write(0x02);              /* head load 4 ms, DMA mode */
}

static bool reset_controller(void)
{
    KeClearEvent(&g_irq_event);
    outb(FDC_DOR, 0x00);
    HalStallMs(1);
    outb(FDC_DOR, g_dor);
    if (!wait_irq(1000))
        return false;
    for (int i = 0; i < 4; i++)
        sense_interrupt(NULL, NULL);
    configure_controller();
    g_drives[0].CurrentCyl = g_drives[1].CurrentCyl = -1;
    return true;
}

static void motor_on(FLOPPY *f)
{
    uint8_t bit = (uint8_t)(0x10 << f->Drive);
    bool was_on = g_dor & bit;
    g_dor = (uint8_t)((g_dor & 0xFC) | bit | f->Drive);
    outb(FDC_DOR, g_dor);
    if (!was_on)
        KeSleepMs(500);           /* spin-up */
    g_last_use = KeQueryUptimeNs();
}

static bool recalibrate(FLOPPY *f)
{
    for (int tries = 0; tries < 2; tries++) {     /* 80-track drives may need two */
        KeClearEvent(&g_irq_event);
        if (!fdc_write(CMD_RECALIBRATE) || !fdc_write((uint8_t)f->Drive))
            return false;
        if (!wait_irq(3000))
            return false;
        uint8_t st0, cyl;
        if (sense_interrupt(&st0, &cyl) && !(st0 & 0xC0) && cyl == 0) {
            f->CurrentCyl = 0;
            return true;
        }
    }
    return false;
}

static bool seek(FLOPPY *f, uint8_t cyl, uint8_t head)
{
    if (f->CurrentCyl < 0 && !recalibrate(f))
        return false;
    if (f->CurrentCyl == cyl)
        return true;
    KeClearEvent(&g_irq_event);
    if (!fdc_write(CMD_SEEK) || !fdc_write((uint8_t)((head << 2) | f->Drive)) || !fdc_write(cyl))
        return false;
    if (!wait_irq(3000))
        return false;
    uint8_t st0, pcn;
    if (!sense_interrupt(&st0, &pcn) || (st0 & 0xC0) || pcn != cyl) {
        f->CurrentCyl = -1;
        return false;
    }
    f->CurrentCyl = cyl;
    KeSleepMs(15);                /* head settle */
    return true;
}

/* READ DATA / WRITE DATA within one track. Returns the 7 result bytes. */
static NTSTATUS transfer(FLOPPY *f, bool write, uint8_t cyl, uint8_t head, uint8_t sector, uint8_t count)
{
    if (!seek(f, cyl, head))
        return STATUS_IO_DEVICE_ERROR;
    outb(FDC_CCR, f->Rate);
    uint32_t bytes = (uint32_t)count * 512;
    if (!HalIsaDmaStart(2, g_dma_phys, bytes, !write))
        return STATUS_INVALID_PARAMETER;
    KeClearEvent(&g_irq_event);
    uint8_t cmd[9] = { write ? CMD_WRITE : CMD_READ, (uint8_t)((head << 2) | f->Drive), cyl, head, sector,
                       2, (uint8_t)(sector + count - 1), f->Gap, 0xFF };
    for (int i = 0; i < 9; i++)
        if (!fdc_write(cmd[i]))
            return STATUS_IO_DEVICE_ERROR;
    if (!wait_irq(3000)) {
        reset_controller();
        return STATUS_IO_TIMEOUT;
    }
    uint8_t r[7];
    for (int i = 0; i < 7; i++) {
        int v = fdc_read();
        if (v < 0)
            return STATUS_IO_DEVICE_ERROR;
        r[i] = (uint8_t)v;
    }
    if ((r[0] & 0xC0) == 0)
        return STATUS_SUCCESS;
    if (r[1] & 0x02)
        return STATUS_MEDIA_WRITE_PROTECTED;
    if (r[1] & 0x20 || r[2] & 0x20)
        return STATUS_CRC_ERROR;
    if (r[1] & 0x05 || r[2] & 0x01)
        return STATUS_NONEXISTENT_SECTOR;          /* no address mark / sector not found */
    return STATUS_IO_DEVICE_ERROR;
}

/* ---- Media detection --------------------------------------------------------- */

static bool read_id(FLOPPY *f, uint8_t rate)
{
    outb(FDC_CCR, rate);
    KeClearEvent(&g_irq_event);
    if (!fdc_write(CMD_READ_ID) || !fdc_write((uint8_t)f->Drive))
        return false;
    if (!wait_irq(1000)) {
        reset_controller();
        return false;
    }
    uint8_t r[7];
    for (int i = 0; i < 7; i++) {
        int v = fdc_read();
        if (v < 0)
            return false;
        r[i] = (uint8_t)v;
    }
    return (r[0] & 0xC0) == 0;
}

static NTSTATUS detect_media(FLOPPY *f)
{
    f->Known = false;
    if (!recalibrate(f) && !(reset_controller() && recalibrate(f)))
        return STATUS_IO_DEVICE_ERROR;
    /* Rates to try: 1.44 MB, 2.88 MB, 720 KB, with default geometry. */
    static const struct { uint8_t Rate, Spt, Gap, Cyl; } tries[] = {
        { 0x00, 18, 0x1B, 80 }, { 0x03, 36, 0x1B, 80 }, { 0x02, 9, 0x2A, 80 },
    };
    unsigned i;
    for (i = 0; i < ARRAY_SIZE(tries); i++)
        if (read_id(f, tries[i].Rate))
            break;
    if (i == ARRAY_SIZE(tries)) {
        f->Present = false;
        return STATUS_NO_MEDIA_IN_DEVICE;
    }
    f->Rate = tries[i].Rate;
    f->Spt = tries[i].Spt;
    f->Gap = tries[i].Gap;
    f->Cylinders = tries[i].Cyl;
    f->Heads = 2;
    f->Present = true;

    /* A FAT boot sector tells the real geometry. */
    if (NT_SUCCESS(transfer(f, false, 0, 0, 1, 1))) {
        uint8_t *b = g_dma;
        uint16_t bps = (uint16_t)(b[11] | b[12] << 8), spt = (uint16_t)(b[24] | b[25] << 8);
        uint16_t heads = (uint16_t)(b[26] | b[27] << 8), total = (uint16_t)(b[19] | b[20] << 8);
        if (b[510] == 0x55 && b[511] == 0xAA && bps == 512 && spt >= 8 && spt <= 36 && heads >= 1 &&
            heads <= 2 && total && total % (spt * heads) == 0 && total / (spt * heads) <= 84) {
            f->Spt = (uint8_t)spt;
            f->Heads = (uint8_t)heads;
            f->Cylinders = (uint8_t)(total / (spt * heads));
        }
    }
    f->Known = true;
    f->WriteProtected = false;
    uint32_t sectors = (uint32_t)f->Cylinders * f->Heads * f->Spt;
    DbgPrint("Floppy: Floppy%u: medium %u KB (%u cylinders, %u heads, %u sectors/track)", f->Drive,
             sectors / 2, f->Cylinders, f->Heads, f->Spt);
    return STATUS_SUCCESS;
}

/* Called by the disk class before each request. The disk class is told
   about a change only after our lock is released: DiskSetMedia goes into
   the cache, which may itself be waiting to call us. */
static NTSTATUS fd_check_media(void *ctx, bool *changed)
{
    FLOPPY *f = ctx;
    enum { NONE, INSERTED, REMOVED } notify = NONE;
    KeWaitForSingleObject(&g_lock, KE_INFINITE);
    motor_on(f);
    NTSTATUS st = STATUS_SUCCESS;
    if (inb(FDC_DIR) & DIR_CHANGED) {
        /* The change line is cleared by stepping the head with a disk in. */
        f->CurrentCyl = -1;
        seek(f, 1, 0);
        recalibrate(f);
        bool still = inb(FDC_DIR) & DIR_CHANGED;
        f->Known = false;
        if (still) {
            if (f->Present) {
                f->Present = false;
                notify = REMOVED;
            }
            st = STATUS_NO_MEDIA_IN_DEVICE;
        }
    }
    if (NT_SUCCESS(st) && !f->Known) {
        st = detect_media(f);
        notify = NT_SUCCESS(st) ? INSERTED : REMOVED;
    }
    uint32_t sectors = (uint32_t)f->Cylinders * f->Heads * f->Spt;
    g_last_use = KeQueryUptimeNs();
    KeReleaseMutex(&g_lock);

    if (notify != NONE) {
        *changed = true;
        DiskSetMedia(f->Disk, notify == INSERTED, notify == INSERTED ? sectors : 0, 512, false);
    }
    return st;
}

static NTSTATUS rw(FLOPPY *f, bool write, uint64_t lba, uint32_t count, uint8_t *buf)
{
    KeWaitForSingleObject(&g_lock, KE_INFINITE);
    motor_on(f);
    NTSTATUS st = f->Known ? STATUS_SUCCESS : STATUS_NO_MEDIA_IN_DEVICE;
    while (NT_SUCCESS(st) && count) {
        uint32_t per_cyl = (uint32_t)f->Heads * f->Spt;
        uint8_t cyl = (uint8_t)(lba / per_cyl);
        uint8_t head = (uint8_t)((lba % per_cyl) / f->Spt);
        uint8_t sector = (uint8_t)(lba % f->Spt + 1);
        uint32_t n = MIN(count, (uint32_t)(f->Spt - sector + 1));
        if (write)
            memcpy(g_dma, buf, n * 512);
        for (int attempt = 0; attempt < 4; attempt++) {
            st = transfer(f, write, cyl, head, sector, (uint8_t)n);
            if (NT_SUCCESS(st) || st == STATUS_MEDIA_WRITE_PROTECTED)
                break;
            if (attempt == 1)
                f->CurrentCyl = -1;              /* recalibrate before the next try */
            if (attempt == 2)
                reset_controller();
        }
        if (!NT_SUCCESS(st))
            break;
        if (!write)
            memcpy(buf, g_dma, n * 512);
        buf += n * 512;
        lba += n;
        count -= n;
    }
    if (st == STATUS_MEDIA_WRITE_PROTECTED)
        f->WriteProtected = true;
    g_last_use = KeQueryUptimeNs();
    KeReleaseMutex(&g_lock);
    return st;
}

static NTSTATUS fd_read(void *ctx, uint64_t lba, uint32_t count, void *buffer)
{
    return rw(ctx, false, lba, count, buffer);
}

static NTSTATUS fd_write(void *ctx, uint64_t lba, uint32_t count, const void *buffer)
{
    return rw(ctx, true, lba, count, (uint8_t *)buffer);
}

static const DISK_PORT_OPS g_ops = { fd_read, fd_write, NULL, fd_check_media };

/* Switch the motor off after 3 s without use. */
static void motor_thread(void *ctx)
{
    UNUSED(ctx);
    for (;;) {
        KeSleepMs(1000);
        if (!(g_dor & 0x30) || KeQueryUptimeNs() - g_last_use < 3000ull * 1000000)
            continue;
        KeWaitForSingleObject(&g_lock, KE_INFINITE);
        if (KeQueryUptimeNs() - g_last_use >= 3000ull * 1000000) {
            g_dor &= 0x0F;
            outb(FDC_DOR, g_dor);
        }
        KeReleaseMutex(&g_lock);
    }
}

/* ---- Set-up ------------------------------------------------------------------- */

static const char *type_name(uint8_t t)
{
    switch (t) {
    case 1: return "5.25\" 360 KB drive";
    case 2: return "5.25\" 1.2 MB drive";
    case 3: return "3.5\" 720 KB drive";
    case 4: return "3.5\" 1.44 MB drive";
    case 5: return "3.5\" 2.88 MB drive";
    default: return "floppy drive";
    }
}

static NTSTATUS fdc_entry(DRIVER_OBJECT *driver)
{
    UNUSED(driver);
    uint8_t cmos = HalCmosRead(0x10);
    if (!cmos)
        return STATUS_SUCCESS;                  /* no floppy drives */
    if (inb(FDC_MSR) == 0xFF) {
        DbgPrint("Floppy: CMOS lists drives but there is no controller");
        return STATUS_NO_SUCH_DEVICE;
    }
    KeInitializeMutex(&g_lock);
    KeInitializeEvent(&g_irq_event, SynchronizationEvent, false);
    KeInitializeDpc(&g_dpc, irq_dpc, NULL);
    g_dma_phys = MmAllocateIsaDmaBuffer(DMA_BUFFER_SIZE);
    if (!g_dma_phys)
        return STATUS_INSUFFICIENT_RESOURCES;
    g_dma = MmPhysToVirt(g_dma_phys);

    uint8_t vec = KeAllocateVectorAtIrql(4);
    KeConnectInterrupt(vec, irq_isr, NULL);
    HalEnableIsaIrq(6, vec);

    if (!reset_controller()) {
        DbgPrint("Floppy: controller does not respond");
        return STATUS_NO_SUCH_DEVICE;
    }
    fdc_write(CMD_VERSION);
    int ver = fdc_read();
    g_enhanced = ver == 0x90;
    if (g_enhanced)
        configure_controller();

    for (unsigned i = 0; i < 2; i++) {
        uint8_t t = i == 0 ? cmos >> 4 : cmos & 0xF;
        if (!t)
            continue;
        FLOPPY *f = &g_drives[i];
        f->Drive = i;
        f->CmosType = t;
        f->CurrentCyl = -1;
        strlcpy(f->Model, type_name(t), sizeof(f->Model));
        snprintf(f->Bus, sizeof(f->Bus), "floppy controller drive %u%s", i, g_enhanced ? ", 82077AA" : "");
        DISK_PARAMS dp = { 0 };
        dp.Floppy = true;
        dp.Removable = true;
        dp.Model = f->Model;
        dp.Bus = f->Bus;
        dp.BytesPerSector = 512;
        dp.SectorCount = 0;                     /* found on first use */
        dp.MaxTransferSectors = 36;
        dp.Ops = &g_ops;
        dp.Context = f;
        DiskCreate(&dp, &f->Disk);
    }
    PsCreateSystemThread(NULL, motor_thread, NULL, 8, "FloppyMotor", NULL);
    return STATUS_SUCCESS;
}

void FdcInitialize(void)
{
    IoCreateDriver("fdc", fdc_entry);
}
