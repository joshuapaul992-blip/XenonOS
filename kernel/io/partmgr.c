/*
 * Partition manager (\Driver\partmgr).
 *
 * Reads a disk's partition table and creates one volume device per
 * partition:
 *
 *   \Device\HarddiskVolumeK            the partition
 *   \Device\HarddiskN\PartitionM       -> \Device\HarddiskVolumeK
 *   \GLOBAL??\HarddiskVolumeK          -> \Device\HarddiskVolumeK
 *
 * Supported: MBR (primary, extended and logical partitions), GPT (header and
 * entry array CRC32-checked, backup header used if the primary one is bad),
 * and "superfloppy" disks that have a FAT file system and no partition table
 * (common on USB sticks), which become one volume covering the whole disk.
 *
 * Partitions are numbered as on Windows NT: MBR primary partitions first in
 * table order, then logical partitions in chain order; GPT in entry order.
 */
#include "iop.h"
#include "mm.h"
#include "rtl.h"
#include "console.h"

#define MAX_VOLUMES 128
#define MAX_PARTS_PER_DISK 32

typedef struct PART_EXTENSION {
    DEVICE_OBJECT *Disk;
    uint64_t Start;                 /* bytes */
    uint64_t Length;                /* bytes */
    uint32_t VolumeNumber;
    PARTITION_INFORMATION_EX Info;
    char Link[48];                  /* \Device\HarddiskN\PartitionM */
    char GlobalLink[48];            /* \GLOBAL??\HarddiskVolumeK */
} PART_EXTENSION;

static DRIVER_OBJECT *g_driver;
static DEVICE_OBJECT *g_volumes[MAX_VOLUMES];
static unsigned g_next_volume = 1;
static KMUTEX g_lock;

unsigned PartMgrVolumeCount(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < MAX_VOLUMES; i++)
        n += g_volumes[i] != NULL;
    return n;
}

DEVICE_OBJECT *PartMgrGetVolume(unsigned index)
{
    for (unsigned i = 0; i < MAX_VOLUMES; i++)
        if (g_volumes[i] && index-- == 0)
            return g_volumes[i];
    return NULL;
}

const char *PartMgrDiskStyle(DEVICE_OBJECT *disk)
{
    return ((DISK_EXTENSION *)disk->DeviceExtension)->Style;
}

/* ---- Dispatch: forward to the disk with the partition's offset ------------- */

static NTSTATUS complete(IRP *irp, NTSTATUS st, uint64_t info)
{
    irp->IoStatus.Status = st;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp);
    return st;
}

static NTSTATUS part_read_write(DEVICE_OBJECT *dev, IRP *irp)
{
    PART_EXTENSION *x = dev->DeviceExtension;
    IO_STACK_LOCATION *s = IoGetCurrentIrpStackLocation(irp);
    uint64_t off = s->Parameters.Read.ByteOffset;
    uint32_t len = s->Parameters.Read.Length;
    if (len == 0)
        return complete(irp, STATUS_SUCCESS, 0);
    if (off >= x->Length)
        return complete(irp, STATUS_END_OF_FILE, 0);
    if (off + len > x->Length)
        return complete(irp, STATUS_INVALID_PARAMETER, 0);
    IoCopyCurrentIrpStackLocationToNext(irp);
    IoGetNextIrpStackLocation(irp)->Parameters.Read.ByteOffset = off + x->Start;
    return IoCallDriver(x->Disk, irp);
}

static NTSTATUS part_forward(DEVICE_OBJECT *dev, IRP *irp)
{
    PART_EXTENSION *x = dev->DeviceExtension;
    IoCopyCurrentIrpStackLocationToNext(irp);
    return IoCallDriver(x->Disk, irp);
}

static NTSTATUS part_ioctl(DEVICE_OBJECT *dev, IRP *irp)
{
    PART_EXTENSION *x = dev->DeviceExtension;
    IO_STACK_LOCATION *s = IoGetCurrentIrpStackLocation(irp);
    uint32_t outlen = s->Parameters.DeviceIoControl.OutputBufferLength;
    switch (s->Parameters.DeviceIoControl.IoControlCode) {
    case IOCTL_DISK_GET_LENGTH_INFO: {
        if (outlen < sizeof(GET_LENGTH_INFORMATION))
            return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        GET_LENGTH_INFORMATION l = { (int64_t)x->Length };
        memcpy(irp->SystemBuffer, &l, sizeof(l));
        return complete(irp, STATUS_SUCCESS, sizeof(l));
    }
    case IOCTL_DISK_GET_PARTITION_INFO_EX:
        if (outlen < sizeof(PARTITION_INFORMATION_EX))
            return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        memcpy(irp->SystemBuffer, &x->Info, sizeof(x->Info));
        return complete(irp, STATUS_SUCCESS, sizeof(x->Info));
    default:
        return part_forward(dev, irp);   /* geometry, disk info, media checks */
    }
}

static NTSTATUS part_create_close(DEVICE_OBJECT *dev, IRP *irp)
{
    UNUSED(dev);
    return complete(irp, STATUS_SUCCESS, 0);
}

static NTSTATUS partmgr_entry(DRIVER_OBJECT *d)
{
    d->MajorFunction[IRP_MJ_CREATE] = part_create_close;
    d->MajorFunction[IRP_MJ_CLOSE] = part_create_close;
    d->MajorFunction[IRP_MJ_CLEANUP] = part_create_close;
    d->MajorFunction[IRP_MJ_READ] = part_read_write;
    d->MajorFunction[IRP_MJ_WRITE] = part_read_write;
    d->MajorFunction[IRP_MJ_FLUSH_BUFFERS] = part_forward;
    d->MajorFunction[IRP_MJ_DEVICE_CONTROL] = part_ioctl;
    g_driver = d;
    KeInitializeMutex(&g_lock);
    return STATUS_SUCCESS;
}

/* ---- Partition tables ---------------------------------------------------- */

typedef struct {
    uint64_t StartLba, Sectors;
    PARTITION_INFORMATION_EX Info;
} FOUND;

static NTSTATUS read_sectors(DEVICE_OBJECT *disk, uint64_t lba, uint32_t count, void *buf)
{
    DISK_EXTENSION *x = disk->DeviceExtension;
    return CcRead(x->Cache, lba * x->BytesPerSector, count * x->BytesPerSector, buf);
}

static const char *mbr_type_name(uint8_t t)
{
    switch (t) {
    case 0x01: return "FAT12";
    case 0x04: case 0x06: case 0x0E: return "FAT16";
    case 0x0B: case 0x0C: return "FAT32";
    case 0x07: return "NTFS/exFAT";
    case 0x82: return "Linux swap";
    case 0x83: return "Linux";
    case 0xEF: return "EFI system";
    case 0xEE: return "GPT protective";
    default:   return "other";
    }
}

static bool is_extended(uint8_t t) { return t == 0x05 || t == 0x0F || t == 0x85; }

static bool valid_fat_boot_sector(const uint8_t *b)
{
    if (!(b[0] == 0xEB || b[0] == 0xE9) || b[510] != 0x55 || b[511] != 0xAA)
        return false;
    uint16_t bps = (uint16_t)(b[11] | (b[12] << 8));
    uint8_t spc = b[13];
    uint16_t reserved = (uint16_t)(b[14] | (b[15] << 8));
    uint8_t fats = b[16];
    return (bps == 512 || bps == 1024 || bps == 2048 || bps == 4096) && spc && !(spc & (spc - 1)) &&
           reserved >= 1 && (fats == 1 || fats == 2);
}

/* Returns the number of partitions found, or -1 if the MBR is not valid. */
static int parse_mbr(DEVICE_OBJECT *disk, const uint8_t *mbr, FOUND *out, int max, bool *gpt)
{
    DISK_EXTENSION *x = disk->DeviceExtension;
    *gpt = false;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA)
        return -1;
    const uint8_t *e = mbr + 0x1BE;
    int n = 0, used = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *p = e + i * 16;
        uint8_t type = p[4];
        uint32_t start = (uint32_t)(p[8] | p[9] << 8 | p[10] << 16 | (uint32_t)p[11] << 24);
        uint32_t count = (uint32_t)(p[12] | p[13] << 8 | p[14] << 16 | (uint32_t)p[15] << 24);
        if (type == 0)
            continue;
        if (p[0] != 0 && p[0] != 0x80)
            return -1;                       /* not a partition table (boot code) */
        if (type == 0xEE) {
            *gpt = true;
            return 0;
        }
        if (start == 0 || count == 0 || (uint64_t)start + count > x->SectorCount)
            return -1;
        used++;
        if (!is_extended(type) && n < max) {
            out[n].StartLba = start;
            out[n].Sectors = count;
            out[n].Info.PartitionStyle = 0;
            out[n].Info.MbrType = type;
            out[n].Info.BootIndicator = p[0] == 0x80;
            n++;
        }
    }
    if (used == 0)
        return -1;
    /* Logical partitions inside the extended partition. */
    for (int i = 0; i < 4; i++) {
        const uint8_t *p = e + i * 16;
        if (!is_extended(p[4]))
            continue;
        uint32_t ext_start = (uint32_t)(p[8] | p[9] << 8 | p[10] << 16 | (uint32_t)p[11] << 24);
        uint64_t ebr = ext_start;
        uint8_t *buf = ExAllocatePoolWithTag(NonPagedPool, x->BytesPerSector, 'PmBu');
        if (!buf)
            break;
        for (int guard = 0; guard < 64 && n < max; guard++) {
            if (ebr >= x->SectorCount || !NT_SUCCESS(read_sectors(disk, ebr, 1, buf)) ||
                buf[510] != 0x55 || buf[511] != 0xAA)
                break;
            const uint8_t *l = buf + 0x1BE, *next = buf + 0x1CE;
            uint32_t ls = (uint32_t)(l[8] | l[9] << 8 | l[10] << 16 | (uint32_t)l[11] << 24);
            uint32_t lc = (uint32_t)(l[12] | l[13] << 8 | l[14] << 16 | (uint32_t)l[15] << 24);
            if (l[4] && lc && ebr + ls + lc <= x->SectorCount) {
                out[n].StartLba = ebr + ls;
                out[n].Sectors = lc;
                out[n].Info.PartitionStyle = 0;
                out[n].Info.MbrType = l[4];
                n++;
            }
            uint32_t ns = (uint32_t)(next[8] | next[9] << 8 | next[10] << 16 | (uint32_t)next[11] << 24);
            if (!is_extended(next[4]) || ns == 0)
                break;
            ebr = ext_start + ns;
        }
        ExFreePoolWithTag(buf, 'PmBu');
        break;                               /* only one extended partition is allowed */
    }
    return n;
}

static int parse_gpt_at(DEVICE_OBJECT *disk, uint64_t header_lba, FOUND *out, int max)
{
    DISK_EXTENSION *x = disk->DeviceExtension;
    uint32_t bps = x->BytesPerSector;
    uint8_t *hdr = ExAllocatePoolWithTag(NonPagedPool, bps, 'PmBu');
    if (!hdr)
        return -1;
    int n = -1;
    if (!NT_SUCCESS(read_sectors(disk, header_lba, 1, hdr)) || memcmp(hdr, "EFI PART", 8))
        goto out;
    uint32_t hsize = *(uint32_t *)(hdr + 12);
    uint32_t hcrc = *(uint32_t *)(hdr + 16);
    if (hsize < 92 || hsize > bps)
        goto out;
    *(uint32_t *)(hdr + 16) = 0;
    if (RtlCrc32(0, hdr, hsize) != hcrc) {
        DbgPrint("PartMgr: %s: GPT header at LBA %llu has a bad CRC", x->Name, (unsigned long long)header_lba);
        goto out;
    }
    uint64_t entries_lba = *(uint64_t *)(hdr + 72);
    uint32_t count = *(uint32_t *)(hdr + 80);
    uint32_t esize = *(uint32_t *)(hdr + 84);
    uint32_t ecrc = *(uint32_t *)(hdr + 88);
    if (esize < 128 || esize > 1024 || count > 1024)
        goto out;
    uint32_t bytes = count * esize;
    uint32_t sectors = (bytes + bps - 1) / bps;
    uint8_t *ent = ExAllocatePoolWithTag(NonPagedPool, sectors * bps, 'PmBu');
    if (!ent)
        goto out;
    if (!NT_SUCCESS(read_sectors(disk, entries_lba, sectors, ent)) || RtlCrc32(0, ent, bytes) != ecrc) {
        DbgPrint("PartMgr: %s: GPT partition entries have a bad CRC", x->Name);
        ExFreePoolWithTag(ent, 'PmBu');
        goto out;
    }
    n = 0;
    static const uint8_t zero[16];
    for (uint32_t i = 0; i < count && n < max; i++) {
        uint8_t *p = ent + i * esize;
        if (!memcmp(p, zero, 16))
            continue;
        uint64_t first = *(uint64_t *)(p + 32), last = *(uint64_t *)(p + 40);
        if (last < first || last >= x->SectorCount)
            continue;
        out[n].StartLba = first;
        out[n].Sectors = last - first + 1;
        out[n].Info.PartitionStyle = 1;
        memcpy(out[n].Info.GptType, p, 16);
        for (int c = 0; c < 36; c++) {                /* UTF-16LE name -> ASCII */
            uint16_t ch = (uint16_t)(p[56 + c * 2] | (p[57 + c * 2] << 8));
            if (!ch) {
                out[n].Info.Name[c] = 0;
                break;
            }
            out[n].Info.Name[c] = ch < 0x80 ? (char)ch : '?';
        }
        n++;
    }
    ExFreePoolWithTag(ent, 'PmBu');
out:
    ExFreePoolWithTag(hdr, 'PmBu');
    return n;
}

static const char *gpt_type_name(const uint8_t g[16])
{
    static const uint8_t esp[16] = { 0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B };
    static const uint8_t basic[16] = { 0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44, 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 };
    static const uint8_t linux_fs[16] = { 0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47, 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4 };
    if (!memcmp(g, esp, 16)) return "EFI system";
    if (!memcmp(g, basic, 16)) return "Basic data";
    if (!memcmp(g, linux_fs, 16)) return "Linux";
    return "other";
}

/* ---- Volume devices -------------------------------------------------------- */

static void create_volume(DEVICE_OBJECT *disk, const FOUND *f, uint32_t number)
{
    DISK_EXTENSION *dx = disk->DeviceExtension;
    unsigned slot = 0;
    while (slot < MAX_VOLUMES && g_volumes[slot])
        slot++;
    if (slot == MAX_VOLUMES)
        return;
    uint32_t vol = g_next_volume++;
    char name[48];
    snprintf(name, sizeof(name), "\\Device\\HarddiskVolume%u", vol);
    DEVICE_OBJECT *dev;
    if (!NT_SUCCESS(IoCreateDevice(g_driver, sizeof(PART_EXTENSION), name, FILE_DEVICE_DISK,
                                   disk->Characteristics & FILE_REMOVABLE_MEDIA, &dev)))
        return;
    PART_EXTENSION *x = dev->DeviceExtension;
    x->Disk = disk;
    x->Start = f->StartLba * dx->BytesPerSector;
    x->Length = f->Sectors * dx->BytesPerSector;
    x->VolumeNumber = vol;
    x->Info = f->Info;
    x->Info.StartingOffset = (int64_t)x->Start;
    x->Info.PartitionLength = (int64_t)x->Length;
    x->Info.PartitionNumber = number;
    dev->StackSize = (int8_t)(disk->StackSize + 1);
    dev->SectorSize = dx->BytesPerSector;
    snprintf(x->Link, sizeof(x->Link), "\\Device\\Harddisk%u\\Partition%u", dx->Number, number);
    IoCreateSymbolicLink(x->Link, name);
    snprintf(x->GlobalLink, sizeof(x->GlobalLink), "\\GLOBAL??\\HarddiskVolume%u", vol);
    IoCreateSymbolicLink(x->GlobalLink, name);
    g_volumes[slot] = dev;

    const char *type = f->Info.PartitionStyle == 0 ? mbr_type_name(f->Info.MbrType)
                     : f->Info.PartitionStyle == 1 ? gpt_type_name(f->Info.GptType) : "whole disk";
    char size[24];
    uint64_t b = x->Length;
    if (b >= (10ull << 30)) snprintf(size, sizeof(size), "%llu GiB", (unsigned long long)(b >> 30));
    else if (b >= (10ull << 20)) snprintf(size, sizeof(size), "%llu MiB", (unsigned long long)(b >> 20));
    else snprintf(size, sizeof(size), "%llu KiB", (unsigned long long)(b >> 10));
    if (f->Info.PartitionStyle == 0)
        DbgPrint("PartMgr: Harddisk%u\\Partition%u: %s (0x%02x)%s, LBA %llu, %s -> HarddiskVolume%u", dx->Number,
                 number, type, f->Info.MbrType, f->Info.BootIndicator ? ", active" : "",
                 (unsigned long long)f->StartLba, size, vol);
    else
        DbgPrint("PartMgr: Harddisk%u\\Partition%u: %s%s%s%s, LBA %llu, %s -> HarddiskVolume%u", dx->Number, number,
                 type, f->Info.Name[0] ? " \"" : "", f->Info.Name, f->Info.Name[0] ? "\"" : "",
                 (unsigned long long)f->StartLba, size, vol);
}

void PartMgrScanDisk(DEVICE_OBJECT *disk)
{
    if (!g_driver)
        IoCreateDriver("partmgr", partmgr_entry);
    DISK_EXTENSION *x = disk->DeviceExtension;
    if (!x->MediaPresent || x->SectorCount < 2)
        return;
    uint8_t *mbr = ExAllocatePoolWithTag(NonPagedPool, x->BytesPerSector, 'PmBu');
    FOUND *found = ExAllocatePoolWithTag(NonPagedPoolZeroed, sizeof(FOUND) * MAX_PARTS_PER_DISK, 'PmBu');
    if (!mbr || !found)
        goto out;
    if (!NT_SUCCESS(read_sectors(disk, 0, 1, mbr))) {
        DbgPrint("PartMgr: %s: cannot read sector 0", x->Name);
        goto out;
    }
    KeWaitForSingleObject(&g_lock, KE_INFINITE);
    bool gpt;
    int n = parse_mbr(disk, mbr, found, MAX_PARTS_PER_DISK, &gpt);
    if (gpt) {
        n = parse_gpt_at(disk, 1, found, MAX_PARTS_PER_DISK);
        if (n < 0) {
            DbgPrint("PartMgr: %s: primary GPT damaged, trying the backup", x->Name);
            n = parse_gpt_at(disk, x->SectorCount - 1, found, MAX_PARTS_PER_DISK);
        }
        strlcpy(x->Style, n >= 0 ? "GPT" : "?", sizeof(x->Style));
    } else if (n >= 0) {
        strlcpy(x->Style, "MBR", sizeof(x->Style));
    } else if (valid_fat_boot_sector(mbr)) {
        /* No partition table, but a FAT file system starts at sector 0. */
        found[0].StartLba = 0;
        found[0].Sectors = x->SectorCount;
        found[0].Info.PartitionStyle = 2;
        n = 1;
        strlcpy(x->Style, "none", sizeof(x->Style));
    } else {
        strlcpy(x->Style, "?", sizeof(x->Style));
        DbgPrint("PartMgr: Harddisk%u: no partition table or file system found", x->Number);
    }
    for (int i = 0; i < n; i++)
        create_volume(disk, &found[i], (uint32_t)i + 1);
    if (n == 0)
        DbgPrint("PartMgr: Harddisk%u: %s partition table with no partitions", x->Number, x->Style);
    KeReleaseMutex(&g_lock);
out:
    if (mbr)
        ExFreePoolWithTag(mbr, 'PmBu');
    if (found)
        ExFreePoolWithTag(found, 'PmBu');
}

void PartMgrRemoveDisk(DEVICE_OBJECT *disk)
{
    if (!g_driver)
        return;
    KeWaitForSingleObject(&g_lock, KE_INFINITE);
    for (unsigned i = 0; i < MAX_VOLUMES; i++) {
        DEVICE_OBJECT *v = g_volumes[i];
        if (!v || ((PART_EXTENSION *)v->DeviceExtension)->Disk != disk)
            continue;
        PART_EXTENSION *x = v->DeviceExtension;
        IoDeleteSymbolicLink(x->Link);
        IoDeleteSymbolicLink(x->GlobalLink);
        g_volumes[i] = NULL;
        DbgPrint("PartMgr: HarddiskVolume%u removed", x->VolumeNumber);
        IoDeleteDevice(v);
    }
    KeReleaseMutex(&g_lock);
}
