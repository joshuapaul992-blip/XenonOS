/* SPDX-License-Identifier: MIT */
/*
 * Disk class driver (\Driver\disk).
 *
 * Storage port drivers (IDE, AHCI, floppy, and later USB) describe each drive
 * with a DISK_PARAMS and get a disk device:
 *
 *   \Device\HarddiskN\DR0        the whole disk
 *   \Device\HarddiskN\Partition0 -> \Device\HarddiskN\DR0
 *   \GLOBAL??\PhysicalDriveN     -> \Device\HarddiskN\DR0
 *   \Device\FloppyN              floppy drives
 *   \GLOBAL??\FloppyN            -> \Device\FloppyN
 *
 * The disk device accepts byte-addressed reads and writes (sector aligned),
 * passes them through the block cache, and answers the standard disk IOCTLs.
 */
#include "iop.h"
#include "mm.h"
#include "rtl.h"
#include "console.h"

#define MAX_DISKS 64

static DRIVER_OBJECT *g_driver;
static DEVICE_OBJECT *g_disks[MAX_DISKS];
static unsigned g_disk_count;
static unsigned g_next_hd, g_next_fd;

unsigned DiskCount(void) { return g_disk_count; }
DEVICE_OBJECT *DiskGet(unsigned i) { return i < g_disk_count ? g_disks[i] : NULL; }

/* ---- Raw access (port driver) ------------------------------------------- */

static NTSTATUS raw(DEVICE_OBJECT *disk, bool write, uint64_t offset, uint32_t length, void *buffer)
{
    DISK_EXTENSION *x = disk->DeviceExtension;
    uint32_t bps = x->BytesPerSector;
    if (!x->MediaPresent)
        return STATUS_NO_MEDIA_IN_DEVICE;
    if (offset % bps || length % bps)
        return STATUS_INVALID_PARAMETER;
    uint64_t lba = offset / bps;
    uint32_t count = length / bps;
    if (lba + count > x->SectorCount)
        return STATUS_NONEXISTENT_SECTOR;
    uint32_t max = x->Params.MaxTransferSectors ? x->Params.MaxTransferSectors : 128;
    uint8_t *p = buffer;
    NTSTATUS st = STATUS_SUCCESS;
    KeWaitForSingleObject(&x->PortLock, KE_INFINITE);
    while (count) {
        uint32_t n = MIN(count, max);
        st = write ? x->Params.Ops->Write(x->Params.Context, lba, n, p)
                   : x->Params.Ops->Read(x->Params.Context, lba, n, p);
        if (!NT_SUCCESS(st))
            break;
        lba += n;
        count -= n;
        p += (uint64_t)n * bps;
    }
    KeReleaseMutex(&x->PortLock);
    if (st == STATUS_MEDIA_WRITE_PROTECTED)
        x->WriteProtected = true;
    return st;
}

NTSTATUS DiskRawRead(DEVICE_OBJECT *disk, uint64_t offset, uint32_t length, void *buffer)
{
    return raw(disk, false, offset, length, buffer);
}

NTSTATUS DiskRawWrite(DEVICE_OBJECT *disk, uint64_t offset, uint32_t length, const void *buffer)
{
    return raw(disk, true, offset, length, (void *)buffer);
}

/* ---- Media ---------------------------------------------------------------- */

void DiskSetMedia(DEVICE_OBJECT *disk, bool present, uint64_t sectors, uint32_t bps, bool write_protected)
{
    DISK_EXTENSION *x = disk->DeviceExtension;
    bool changed = present != x->MediaPresent || sectors != x->SectorCount || bps != x->BytesPerSector;
    if (x->Cache)
        CcInvalidate(x->Cache);
    x->MediaChangeCount++;             /* any call means the medium may be different */
    x->MediaPresent = present;
    x->SectorCount = present ? sectors : 0;
    if (bps)
        x->BytesPerSector = bps;
    disk->SectorSize = x->BytesPerSector;
    x->WriteProtected = write_protected;
    if (changed && x->Name[0])
        DbgPrint("Disk: %s: %s", x->Name, present ? "medium inserted" : "no medium");
    if (!x->Params.Floppy) {
        PartMgrRemoveDisk(disk);
        if (present)
            PartMgrScanDisk(disk);
    }
}

/* Removable media: ask the port driver whether the medium changed. */
static NTSTATUS check_media(DEVICE_OBJECT *disk)
{
    DISK_EXTENSION *x = disk->DeviceExtension;
    if (!x->Params.Removable || !x->Params.Ops->CheckMedia)
        return x->MediaPresent ? STATUS_SUCCESS : STATUS_NO_MEDIA_IN_DEVICE;
    /* Not under PortLock: the port driver serialises itself, and it calls
       DiskSetMedia (which takes the cache lock) when something changed. A
       reader holding the cache lock may be waiting for PortLock. */
    bool changed = false;
    NTSTATUS st = x->Params.Ops->CheckMedia(x->Params.Context, &changed);
    if (!NT_SUCCESS(st))
        return st;
    return x->MediaPresent ? STATUS_SUCCESS : STATUS_NO_MEDIA_IN_DEVICE;
}

/* ---- Dispatch --------------------------------------------------------------- */

static NTSTATUS complete(IRP *irp, NTSTATUS st, uint64_t info)
{
    irp->IoStatus.Status = st;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp);
    return st;
}

static NTSTATUS disk_read_write(DEVICE_OBJECT *dev, IRP *irp)
{
    DISK_EXTENSION *x = dev->DeviceExtension;
    IO_STACK_LOCATION *s = IoGetCurrentIrpStackLocation(irp);
    bool write = s->MajorFunction == IRP_MJ_WRITE;
    uint64_t off = s->Parameters.Read.ByteOffset;
    uint32_t len = s->Parameters.Read.Length;

    NTSTATUS st = check_media(dev);
    if (!NT_SUCCESS(st))
        return complete(irp, st, 0);
    uint64_t size = x->SectorCount * x->BytesPerSector;
    if (off % x->BytesPerSector || len % x->BytesPerSector)
        return complete(irp, STATUS_INVALID_PARAMETER, 0);
    if (len == 0)
        return complete(irp, STATUS_SUCCESS, 0);
    if (off >= size)
        return complete(irp, STATUS_END_OF_FILE, 0);
    if (off + len > size)
        return complete(irp, STATUS_INVALID_PARAMETER, 0);
    if (write && x->WriteProtected)
        return complete(irp, STATUS_MEDIA_WRITE_PROTECTED, 0);

    st = write ? CcWrite(x->Cache, off, len, irp->SystemBuffer) : CcRead(x->Cache, off, len, irp->SystemBuffer);
    return complete(irp, st, NT_SUCCESS(st) ? len : 0);
}

static NTSTATUS disk_flush(DEVICE_OBJECT *dev, IRP *irp)
{
    DISK_EXTENSION *x = dev->DeviceExtension;
    NTSTATUS st = x->Cache ? CcFlush(x->Cache) : STATUS_SUCCESS;
    if (NT_SUCCESS(st) && x->Params.Ops->Flush && x->MediaPresent) {
        KeWaitForSingleObject(&x->PortLock, KE_INFINITE);
        st = x->Params.Ops->Flush(x->Params.Context);
        KeReleaseMutex(&x->PortLock);
    }
    return complete(irp, st, 0);
}

static NTSTATUS disk_ioctl(DEVICE_OBJECT *dev, IRP *irp)
{
    DISK_EXTENSION *x = dev->DeviceExtension;
    IO_STACK_LOCATION *s = IoGetCurrentIrpStackLocation(irp);
    uint32_t outlen = s->Parameters.DeviceIoControl.OutputBufferLength;
    void *buf = irp->SystemBuffer;

    switch (s->Parameters.DeviceIoControl.IoControlCode) {
    case IOCTL_STORAGE_CHECK_VERIFY:
        return complete(irp, check_media(dev), 0);
    case IOCTL_DISK_GET_DRIVE_GEOMETRY: {
        if (outlen < sizeof(DISK_GEOMETRY))
            return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        NTSTATUS st = check_media(dev);
        if (!NT_SUCCESS(st))
            return complete(irp, st, 0);
        DISK_GEOMETRY g = { 0 };
        g.BytesPerSector = x->BytesPerSector;
        if (x->Params.Floppy) {
            g.SectorsPerTrack = x->SectorCount == 1440 ? 9 : x->SectorCount == 5760 ? 36 : 18;
            g.TracksPerCylinder = 2;
            g.MediaType = x->SectorCount == 2880 ? 2 : 0;
        } else {
            g.SectorsPerTrack = 63;
            g.TracksPerCylinder = 255;
            g.MediaType = x->Params.Removable ? 11 : 12;
        }
        g.Cylinders = (int64_t)(x->SectorCount / (g.SectorsPerTrack * g.TracksPerCylinder));
        memcpy(buf, &g, sizeof(g));
        return complete(irp, STATUS_SUCCESS, sizeof(g));
    }
    case IOCTL_DISK_GET_LENGTH_INFO: {
        if (outlen < sizeof(GET_LENGTH_INFORMATION))
            return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        NTSTATUS st = check_media(dev);
        if (!NT_SUCCESS(st))
            return complete(irp, st, 0);
        GET_LENGTH_INFORMATION l = { (int64_t)(x->SectorCount * x->BytesPerSector) };
        memcpy(buf, &l, sizeof(l));
        return complete(irp, STATUS_SUCCESS, sizeof(l));
    }
    case IOCTL_DISK_GET_PARTITION_INFO_EX: {
        if (outlen < sizeof(PARTITION_INFORMATION_EX))
            return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        PARTITION_INFORMATION_EX p = { 0 };
        p.PartitionStyle = 2;
        p.PartitionLength = (int64_t)(x->SectorCount * x->BytesPerSector);
        memcpy(buf, &p, sizeof(p));
        return complete(irp, STATUS_SUCCESS, sizeof(p));
    }
    case IOCTL_XENON_QUERY_DISK: {
        if (outlen < sizeof(XENON_DISK_INFO))
            return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        if (x->Params.Removable)
            check_media(dev);
        XENON_DISK_INFO i = { 0 };
        strlcpy(i.Model, x->Model, sizeof(i.Model));
        strlcpy(i.Serial, x->Serial, sizeof(i.Serial));
        strlcpy(i.Bus, x->Bus, sizeof(i.Bus));
        i.BytesPerSector = x->BytesPerSector;
        i.SectorCount = x->SectorCount;
        i.Removable = x->Params.Removable;
        i.MediaPresent = x->MediaPresent;
        i.WriteProtected = x->WriteProtected;
        i.DiskNumber = x->Number;
        i.Floppy = x->Params.Floppy;
        i.MediaChangeCount = x->MediaChangeCount;
        memcpy(buf, &i, sizeof(i));
        return complete(irp, STATUS_SUCCESS, sizeof(i));
    }
    default:
        return complete(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

static NTSTATUS disk_create_close(DEVICE_OBJECT *dev, IRP *irp)
{
    UNUSED(dev);
    return complete(irp, STATUS_SUCCESS, 0);
}

static NTSTATUS disk_driver_entry(DRIVER_OBJECT *d)
{
    d->MajorFunction[IRP_MJ_CREATE] = disk_create_close;
    d->MajorFunction[IRP_MJ_CLOSE] = disk_create_close;
    d->MajorFunction[IRP_MJ_CLEANUP] = disk_create_close;
    d->MajorFunction[IRP_MJ_READ] = disk_read_write;
    d->MajorFunction[IRP_MJ_WRITE] = disk_read_write;
    d->MajorFunction[IRP_MJ_FLUSH_BUFFERS] = disk_flush;
    d->MajorFunction[IRP_MJ_SHUTDOWN] = disk_flush;
    d->MajorFunction[IRP_MJ_DEVICE_CONTROL] = disk_ioctl;
    g_driver = d;
    return STATUS_SUCCESS;
}

/* ---- Creating disks --------------------------------------------------------- */

static void format_size(uint64_t bytes, char *out, size_t n)
{
    if (bytes >= (10ull << 30))
        snprintf(out, n, "%llu GiB", (unsigned long long)(bytes >> 30));
    else if (bytes >= (10ull << 20))
        snprintf(out, n, "%llu MiB", (unsigned long long)(bytes >> 20));
    else
        snprintf(out, n, "%llu KiB", (unsigned long long)(bytes >> 10));
}

NTSTATUS DiskCreate(const DISK_PARAMS *p, DEVICE_OBJECT **out)
{
    if (!g_driver) {
        NTSTATUS st = IoCreateDriver("disk", disk_driver_entry);
        if (!NT_SUCCESS(st))
            return st;
    }
    if (g_disk_count >= MAX_DISKS)
        return STATUS_INSUFFICIENT_RESOURCES;

    char name[48], link[48], target[48];
    unsigned number;
    if (p->Floppy) {
        number = g_next_fd++;
        snprintf(name, sizeof(name), "\\Device\\Floppy%u", number);
    } else {
        number = g_next_hd++;
        snprintf(name, sizeof(name), "\\Device\\Harddisk%u", number);
        ObCreateDirectoryObject(name, NULL);
        snprintf(name, sizeof(name), "\\Device\\Harddisk%u\\DR0", number);
    }

    DEVICE_OBJECT *dev;
    uint32_t ch = (p->Removable ? FILE_REMOVABLE_MEDIA : 0) | (p->Floppy ? FILE_FLOPPY_DISKETTE : 0);
    NTSTATUS st = IoCreateDevice(g_driver, sizeof(DISK_EXTENSION), name, FILE_DEVICE_DISK, ch, &dev);
    if (!NT_SUCCESS(st))
        return st;
    DISK_EXTENSION *x = dev->DeviceExtension;
    x->Params = *p;
    strlcpy(x->Model, p->Model ? p->Model : "", sizeof(x->Model));
    strlcpy(x->Serial, p->Serial ? p->Serial : "", sizeof(x->Serial));
    strlcpy(x->Bus, p->Bus ? p->Bus : "", sizeof(x->Bus));
    strlcpy(x->Name, name, sizeof(x->Name));
    strlcpy(x->Style, p->Floppy ? "-" : "?", sizeof(x->Style));
    x->Params.Model = x->Model;
    x->Params.Serial = x->Serial;
    x->Params.Bus = x->Bus;
    x->Number = number;
    x->BytesPerSector = p->BytesPerSector ? p->BytesPerSector : 512;
    x->SectorCount = p->SectorCount;
    x->MediaPresent = p->SectorCount != 0;
    dev->SectorSize = x->BytesPerSector;
    KeInitializeMutex(&x->PortLock);
    x->Cache = CcCreate(dev, p->Removable);
    if (!x->Cache)
        return STATUS_INSUFFICIENT_RESOURCES;

    if (p->Floppy) {
        snprintf(link, sizeof(link), "\\GLOBAL??\\Floppy%u", number);
        IoCreateSymbolicLink(link, name);
        IoMarkVolumeDevice(dev);       /* a floppy is a volume: file systems mount on it */
    } else {
        snprintf(link, sizeof(link), "\\Device\\Harddisk%u\\Partition0", number);
        IoCreateSymbolicLink(link, name);
        snprintf(link, sizeof(link), "\\GLOBAL??\\PhysicalDrive%u", number);
        snprintf(target, sizeof(target), "\\Device\\Harddisk%u\\DR0", number);
        IoCreateSymbolicLink(link, target);
    }
    g_disks[g_disk_count++] = dev;

    char size[24];
    format_size(x->SectorCount * x->BytesPerSector, size, sizeof(size));
    if (p->Floppy)
        DbgPrint("Disk: Floppy%u: %s (%s)", number, x->Model, x->Bus);
    else
        DbgPrint("Disk: Harddisk%u: %s (%s), %s, %u-byte sectors%s", number, x->Model, x->Bus,
                 x->MediaPresent ? size : "no medium", x->BytesPerSector, p->Removable ? ", removable" : "");

    if (!p->Floppy && x->MediaPresent)
        PartMgrScanDisk(dev);
    if (out)
        *out = dev;
    return STATUS_SUCCESS;
}
