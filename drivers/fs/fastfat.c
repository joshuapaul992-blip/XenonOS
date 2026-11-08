/* SPDX-License-Identifier: MIT */
/*
 * FastFat - the FAT12/16/32 file system driver (\Driver\fastfat).
 *
 * Wraps the portable FAT core (fatcore.c) in the I/O manager's interface:
 * mount and dismount requests, IRP_MJ_CREATE with NT dispositions, read and
 * write, query and set information (attributes, times, size, rename,
 * delete-on-close), directory listing with wildcards, and volume
 * information.
 *
 * One mounted volume = one volume control block (VCB) in the extension of
 * an unnamed device object. The core is not thread safe, so every request on
 * a volume runs under the VCB's mutex. A file object's FsContext is the
 * core's shared node for the file; FsContext2 is a per-open CCB.
 */
#include "io.h"
#include "mm.h"
#include "hal.h"
#include "rtl.h"
#include "console.h"
#include "fatcore.h"

typedef struct VCB {
    fat_volume *Fat;
    fat_dev Dev;
    DEVICE_OBJECT *Real;
    DEVICE_OBJECT *Self;
    VPB *Vpb;
    KMUTEX Lock;
    uint32_t OpenFiles;
    bool Dismounted;
    char FsName[8];
} VCB;

typedef struct CCB {
    uint32_t Cookie;              /* directory listing position */
    char Pattern[256];
    bool DeleteOnClose;
    bool VolumeOpen;
} CCB;

static DRIVER_OBJECT *g_driver;
static DEVICE_OBJECT *g_cdo;     /* \Device\FastFat: receives mount requests */

/* ---- Environment for the FAT core --------------------------------------- */

void *fat_env_alloc(size_t size) { return ExAllocatePoolWithTag(NonPagedPoolZeroed, size, 'FatC'); }
void fat_env_free(void *p) { ExFreePoolWithTag(p, 'FatC'); }

void fat_env_now(fat_time *t)
{
    int64_t nt = KeQuerySystemTime();
    TIME_FIELDS tf;
    RtlUnixToTimeFields(RtlNtTimeToUnix(nt), &tf);
    t->year = tf.Year;
    t->month = tf.Month;
    t->day = tf.Day;
    t->hour = tf.Hour;
    t->minute = tf.Minute;
    t->second = tf.Second;
    t->ms = (uint16_t)((nt / 10000) % 1000);
}

static int dev_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    VCB *v = ctx;
    uint32_t ss = v->Dev.sector_size;
    return NT_SUCCESS(IoSynchronousReadWrite(v->Real, false, buf, count * ss, lba * ss, NULL)) ? 0 : -1;
}

static int dev_write(void *ctx, uint64_t lba, uint32_t count, const void *buf)
{
    VCB *v = ctx;
    uint32_t ss = v->Dev.sector_size;
    return NT_SUCCESS(IoSynchronousReadWrite(v->Real, true, (void *)buf, count * ss, lba * ss, NULL)) ? 0 : -1;
}

static NTSTATUS status_of(int e)
{
    switch (e) {
    case FAT_OK:           return STATUS_SUCCESS;
    case FAT_EIO:          return STATUS_IO_DEVICE_ERROR;
    case FAT_ENOENT:       return STATUS_OBJECT_NAME_NOT_FOUND;
    case FAT_EEXIST:       return STATUS_OBJECT_NAME_COLLISION;
    case FAT_ENOTDIR:      return STATUS_NOT_A_DIRECTORY;
    case FAT_EISDIR:       return STATUS_FILE_IS_A_DIRECTORY;
    case FAT_ENOTEMPTY:    return STATUS_DIRECTORY_NOT_EMPTY;
    case FAT_ENOSPC:       return STATUS_DISK_FULL;
    case FAT_EINVAL:       return STATUS_OBJECT_NAME_INVALID;
    case FAT_ENOMEM:       return STATUS_INSUFFICIENT_RESOURCES;
    case FAT_EROFS:        return STATUS_MEDIA_WRITE_PROTECTED;
    case FAT_ECORRUPT:     return STATUS_FILE_CORRUPT_ERROR;
    case FAT_EBUSY:        return STATUS_SHARING_VIOLATION;
    case FAT_EFBIG:        return STATUS_FILE_TOO_LARGE;
    case FAT_ENAMETOOLONG: return STATUS_NAME_TOO_LONG;
    case FAT_ENOTFAT:      return STATUS_UNRECOGNIZED_VOLUME;
    default:               return STATUS_UNSUCCESSFUL;
    }
}

static int64_t nt_time(const fat_time *t)
{
    TIME_FIELDS tf = { t->year, t->month, t->day, t->hour, t->minute, t->second, 0 };
    return RtlUnixToNtTime(RtlTimeFieldsToUnix(&tf)) + (int64_t)t->ms * 10000;
}

static void fat_time_of(int64_t nt, fat_time *t)
{
    TIME_FIELDS tf;
    RtlUnixToTimeFields(RtlNtTimeToUnix(nt), &tf);
    t->year = tf.Year;
    t->month = tf.Month;
    t->day = tf.Day;
    t->hour = tf.Hour;
    t->minute = tf.Minute;
    t->second = tf.Second;
    t->ms = (uint16_t)((nt / 10000) % 1000);
}

/* ---- Helpers ----------------------------------------------------------------- */

static NTSTATUS complete(IRP *irp, NTSTATUS st, uint64_t info)
{
    irp->IoStatus.Status = st;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp);
    return st;
}

/* "\a\b\c" -> parent "a\b", leaf "c". */
static void split_path(const char *path, char *parent, size_t psize, const char **leaf)
{
    while (*path == '\\')
        path++;
    const char *slash = strrchr(path, '\\');
    if (!slash) {
        parent[0] = 0;
        *leaf = path;
        return;
    }
    size_t n = MIN((size_t)(slash - path), psize - 1);
    memcpy(parent, path, n);
    parent[n] = 0;
    *leaf = slash + 1;
}

static bool wild_match(const char *pat, const char *s)
{
    if (!strcmp(pat, "*") || !strcmp(pat, "*.*"))
        return true;
    const char *star = NULL, *mark = NULL;
    while (*s) {
        if (*pat == '?' || tolower((unsigned char)*pat) == tolower((unsigned char)*s)) {
            pat++;
            s++;
        } else if (*pat == '*') {
            star = pat++;
            mark = s;
        } else if (star) {
            pat = star + 1;
            s = ++mark;
        } else {
            return false;
        }
    }
    while (*pat == '*')
        pat++;
    return *pat == 0;
}

static bool wants_write(uint32_t access)
{
    return access & (FILE_WRITE_DATA | FILE_APPEND_DATA | GENERIC_WRITE | GENERIC_ALL);
}

/* ---- Mount and dismount ---------------------------------------------------------- */

static NTSTATUS mount(VPB *vpb, DEVICE_OBJECT *real)
{
    GET_LENGTH_INFORMATION len;
    NTSTATUS st = IoSynchronousDeviceControl(real, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &len, sizeof(len), NULL);
    if (!NT_SUCCESS(st))
        return st;
    XENON_DISK_INFO di;
    bool ro = NT_SUCCESS(IoSynchronousDeviceControl(real, IOCTL_XENON_QUERY_DISK, NULL, 0, &di, sizeof(di), NULL)) &&
              di.WriteProtected;

    DEVICE_OBJECT *vdo;
    st = IoCreateDevice(g_driver, sizeof(VCB), NULL, FILE_DEVICE_DISK_FILE_SYSTEM, 0, &vdo);
    if (!NT_SUCCESS(st))
        return st;
    VCB *v = vdo->DeviceExtension;
    v->Real = real;
    v->Self = vdo;
    v->Vpb = vpb;
    KeInitializeMutex(&v->Lock);
    uint32_t ss = real->SectorSize ? real->SectorSize : 512;
    v->Dev = (fat_dev){ dev_read, dev_write, v, ss, (uint64_t)len.Length / ss };
    int e = fat_mount(&v->Dev, ro, &v->Fat);
    if (e) {
        IoDeleteDevice(vdo);
        return e == FAT_ENOTFAT ? STATUS_UNRECOGNIZED_VOLUME : status_of(e);
    }
    fat_volinfo vi;
    fat_volume_info(v->Fat, &vi);
    snprintf(v->FsName, sizeof(v->FsName), "FAT%d", vi.type);
    vpb->DeviceObject = vdo;
    vpb->SerialNumber = vi.serial;
    strlcpy(vpb->VolumeLabel, vi.label, sizeof(vpb->VolumeLabel));
    strlcpy(vpb->FileSystem, v->FsName, sizeof(vpb->FileSystem));
    char path[64];
    ObQueryNameString(real, path, sizeof(path));
    DbgPrint("FastFat: %s mounted on %s: label \"%s\", serial %04X-%04X, %u-byte clusters, %llu MiB free%s",
             v->FsName, path, vi.label, vi.serial >> 16, vi.serial & 0xFFFF, vi.cluster_bytes,
             (unsigned long long)((uint64_t)vi.free_clusters * vi.cluster_bytes >> 20), ro ? ", read-only" : "");
    return STATUS_SUCCESS;
}

static void teardown_nowrite(VCB *v)
{
    if (v->Fat) {
        fat_discard(v->Fat);
        v->Fat = NULL;
    }
}

static void teardown(VCB *v)
{
    if (!v->Fat)
        return;
    int e = fat_unmount(v->Fat);
    v->Fat = NULL;
    if (e)
        DbgPrint("FastFat: unmount: %s", fat_strerror(e));
    IoSynchronousFlush(v->Real);
}

static NTSTATUS dismount(VCB *v, bool force, bool nowrite)
{
    KeWaitForSingleObject(&v->Lock, KE_INFINITE);
    if (!force && v->OpenFiles) {
        KeReleaseMutex(&v->Lock);
        return STATUS_SHARING_VIOLATION;
    }
    v->Dismounted = true;
    bool now = v->OpenFiles == 0;
    /* A forced dismount writes everything and marks the volume clean even
       with files open; requests on those files then fail with
       STATUS_VOLUME_DISMOUNTED. */
    if (nowrite)
        teardown_nowrite(v);
    else if (now || force)
        teardown(v);
    KeReleaseMutex(&v->Lock);
    if (now) {
        v->Vpb->DeviceObject = NULL;
        IoDeleteDevice(v->Self);
    }
    /* Otherwise the volume goes away when its last file is closed. */
    return STATUS_SUCCESS;
}

static NTSTATUS fsd_fs_control(DEVICE_OBJECT *dev, IRP *irp)
{
    IO_STACK_LOCATION *s = IoGetCurrentIrpStackLocation(irp);
    if (s->MinorFunction == IRP_MN_MOUNT_VOLUME && dev == g_cdo)
        return complete(irp, mount(s->Parameters.FileSystemControl.Vpb, s->Parameters.FileSystemControl.DeviceObject), 0);
    uint32_t code = s->Parameters.FileSystemControl.FsControlCode;
    if (s->MinorFunction == IRP_MN_USER_FS_REQUEST && dev != g_cdo &&
        (code == FSCTL_DISMOUNT_VOLUME || code == FSCTL_XENON_FORCE_DISMOUNT || code == FSCTL_XENON_INVALIDATE)) {
        VCB *v = dev->DeviceExtension;
        return complete(irp, dismount(v, code != FSCTL_DISMOUNT_VOLUME, code == FSCTL_XENON_INVALIDATE), 0);
    }
    return complete(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

/* ---- Create ------------------------------------------------------------------------ */

static NTSTATUS do_create(VCB *v, IRP *irp, uint64_t *info)
{
    IO_STACK_LOCATION *s = IoGetCurrentIrpStackLocation(irp);
    FILE_OBJECT *f = s->FileObject;
    uint32_t disposition = s->Parameters.Create.Options >> 24;
    uint32_t options = s->Parameters.Create.Options & 0x00FFFFFF;
    uint32_t access = s->Parameters.Create.DesiredAccess;
    uint32_t attrs = s->Parameters.Create.FileAttributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN |
                                                            FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE);
    CCB *ccb = ExAllocatePoolWithTag(NonPagedPoolZeroed, sizeof(CCB), 'FatX');
    if (!ccb)
        return STATUS_INSUFFICIENT_RESOURCES;
    strlcpy(ccb->Pattern, "*", sizeof(ccb->Pattern));

    if (f->Flags & FO_VOLUME_OPEN) {
        ccb->VolumeOpen = true;
        f->FsContext2 = ccb;
        *info = FILE_OPENED;
        return STATUS_SUCCESS;
    }

    char parent_path[512];
    const char *leaf;
    split_path(f->FileName, parent_path, sizeof(parent_path), &leaf);
    fat_node *node = NULL;
    int e;
    NTSTATUS st = STATUS_SUCCESS;

    if (!*leaf) {
        /* The root directory. */
        node = fat_root(v->Fat);
        if (options & FILE_NON_DIRECTORY_FILE)
            st = STATUS_FILE_IS_A_DIRECTORY;
        else if (disposition == FILE_CREATE)
            st = STATUS_OBJECT_NAME_COLLISION;
        *info = FILE_OPENED;
    } else {
        fat_node *parent;
        e = fat_open(v->Fat, parent_path, &parent);
        if (e) {
            ExFreePoolWithTag(ccb, 'FatX');
            return e == FAT_ENOENT ? STATUS_OBJECT_PATH_NOT_FOUND : status_of(e);
        }
        if (!fat_is_dir(parent)) {
            fat_close(parent);
            ExFreePoolWithTag(ccb, 'FatX');
            return STATUS_OBJECT_PATH_NOT_FOUND;
        }
        e = fat_lookup(parent, leaf, &node);
        if (e == FAT_OK) {
            fat_info fi;
            fat_stat(node, &fi);
            bool dir = fat_is_dir(node);
            if (disposition == FILE_CREATE)
                st = STATUS_OBJECT_NAME_COLLISION;
            else if (dir && (options & FILE_NON_DIRECTORY_FILE))
                st = STATUS_FILE_IS_A_DIRECTORY;
            else if (!dir && (options & FILE_DIRECTORY_FILE))
                st = STATUS_NOT_A_DIRECTORY;
            else if ((fi.attr & FAT_ATTR_READONLY) && (wants_write(access) || disposition >= FILE_OVERWRITE ||
                                                      disposition == FILE_SUPERSEDE))
                st = STATUS_ACCESS_DENIED;
            else if ((fi.attr & FAT_ATTR_READONLY) && (options & FILE_DELETE_ON_CLOSE))
                st = STATUS_CANNOT_DELETE;
            else if (disposition == FILE_OVERWRITE || disposition == FILE_OVERWRITE_IF || disposition == FILE_SUPERSEDE) {
                if (dir) {
                    st = STATUS_FILE_IS_A_DIRECTORY;
                } else {
                    st = status_of(fat_truncate(node, 0));
                    if (NT_SUCCESS(st) && attrs)
                        fat_set_attr(node, (uint8_t)(attrs | FAT_ATTR_ARCHIVE));
                    *info = disposition == FILE_SUPERSEDE ? FILE_SUPERSEDED : FILE_OVERWRITTEN;
                }
            } else {
                *info = FILE_OPENED;
            }
        } else if (e == FAT_ENOENT) {
            if (disposition == FILE_OPEN || disposition == FILE_OVERWRITE) {
                st = STATUS_OBJECT_NAME_NOT_FOUND;
            } else {
                e = fat_create(parent, leaf, (options & FILE_DIRECTORY_FILE) != 0, &node);
                st = status_of(e);
                if (NT_SUCCESS(st) && attrs && !(options & FILE_DIRECTORY_FILE))
                    fat_set_attr(node, (uint8_t)attrs);
                *info = FILE_CREATED;
            }
        } else {
            st = status_of(e);
        }
        fat_close(parent);
    }
    if (!NT_SUCCESS(st)) {
        if (node)
            fat_close(node);
        ExFreePoolWithTag(ccb, 'FatX');
        return st;
    }
    ccb->DeleteOnClose = (options & FILE_DELETE_ON_CLOSE) != 0;
    f->FsContext = node;
    f->FsContext2 = ccb;
    if (fat_is_dir(node))
        f->Flags |= FO_DIRECTORY;
    return STATUS_SUCCESS;
}

/* ---- Information ---------------------------------------------------------------- */

static NTSTATUS query_info(VCB *v, FILE_OBJECT *f, uint32_t cls, void *buf, uint32_t len, uint64_t *info)
{
    fat_node *n = f->FsContext;
    if (!n)
        return STATUS_INVALID_PARAMETER;
    fat_info fi;
    int e = fat_stat(n, &fi);
    if (e)
        return status_of(e);
    fat_volinfo vi;
    fat_volume_info(v->Fat, &vi);
    switch (cls) {
    case FileBasicInformation: {
        if (len < sizeof(FILE_BASIC_INFORMATION))
            return STATUS_BUFFER_TOO_SMALL;
        FILE_BASIC_INFORMATION *b = buf;
        b->CreationTime = nt_time(&fi.created);
        b->LastAccessTime = nt_time(&fi.accessed);
        b->LastWriteTime = b->ChangeTime = nt_time(&fi.modified);
        b->FileAttributes = fi.attr ? fi.attr : FILE_ATTRIBUTE_NORMAL;
        *info = sizeof(*b);
        return STATUS_SUCCESS;
    }
    case FileStandardInformation: {
        if (len < sizeof(FILE_STANDARD_INFORMATION))
            return STATUS_BUFFER_TOO_SMALL;
        FILE_STANDARD_INFORMATION *si = buf;
        si->EndOfFile = fi.size;
        si->AllocationSize = ((int64_t)fi.size + vi.cluster_bytes - 1) / vi.cluster_bytes * vi.cluster_bytes;
        si->NumberOfLinks = 1;
        si->DeletePending = ((CCB *)f->FsContext2)->DeleteOnClose;
        si->Directory = fat_is_dir(n);
        *info = sizeof(*si);
        return STATUS_SUCCESS;
    }
    case FileNameInformation: {
        uint32_t nl = (uint32_t)strlen(f->FileName);
        if (len < sizeof(FILE_NAME_INFORMATION) + nl)
            return STATUS_BUFFER_TOO_SMALL;
        FILE_NAME_INFORMATION *ni = buf;
        ni->FileNameLength = nl;
        memcpy(ni->FileName, f->FileName, nl + 1);
        *info = sizeof(*ni) + nl;
        return STATUS_SUCCESS;
    }
    default:
        return STATUS_INVALID_PARAMETER;
    }
}

static NTSTATUS set_info(VCB *v, FILE_OBJECT *f, IO_STACK_LOCATION *s, void *buf)
{
    fat_node *n = f->FsContext;
    CCB *ccb = f->FsContext2;
    if (!n)
        return STATUS_INVALID_PARAMETER;
    uint32_t len = s->Parameters.SetFile.Length;
    switch (s->Parameters.SetFile.FileInformationClass) {
    case FileBasicInformation: {
        if (len < sizeof(FILE_BASIC_INFORMATION))
            return STATUS_INVALID_PARAMETER;
        FILE_BASIC_INFORMATION *b = buf;
        int e = FAT_OK;
        if (b->FileAttributes && b->FileAttributes != FILE_ATTRIBUTE_NORMAL)
            e = fat_set_attr(n, (uint8_t)b->FileAttributes);
        else if (b->FileAttributes == FILE_ATTRIBUTE_NORMAL)
            e = fat_set_attr(n, 0);
        if (!e && (b->CreationTime || b->LastWriteTime || b->LastAccessTime)) {
            fat_time c, m, a;
            fat_time_of(b->CreationTime, &c);
            fat_time_of(b->LastWriteTime, &m);
            fat_time_of(b->LastAccessTime, &a);
            e = fat_set_times(n, b->CreationTime ? &c : NULL, b->LastWriteTime ? &m : NULL,
                              b->LastAccessTime ? &a : NULL);
        }
        return status_of(e);
    }
    case FileEndOfFileInformation:
        if (len < sizeof(FILE_END_OF_FILE_INFORMATION))
            return STATUS_INVALID_PARAMETER;
        if (!wants_write(f->DesiredAccess))
            return STATUS_ACCESS_DENIED;
        return status_of(fat_truncate(n, (uint64_t)((FILE_END_OF_FILE_INFORMATION *)buf)->EndOfFile));
    case FileDispositionInformation: {
        if (len < sizeof(FILE_DISPOSITION_INFORMATION))
            return STATUS_INVALID_PARAMETER;
        bool del = ((FILE_DISPOSITION_INFORMATION *)buf)->DeleteFile;
        if (del) {
            fat_info fi;
            fat_stat(n, &fi);
            if (!fat_parent(n))
                return STATUS_CANNOT_DELETE;               /* the root */
            if (fi.attr & FAT_ATTR_READONLY)
                return STATUS_CANNOT_DELETE;
            if (fat_is_dir(n)) {
                uint32_t cookie = 0;
                fat_info ci;
                while (fat_readdir(n, &cookie, &ci) == 1)
                    if (strcmp(ci.name, ".") && strcmp(ci.name, ".."))
                        return STATUS_DIRECTORY_NOT_EMPTY;
            }
        }
        ccb->DeleteOnClose = del;
        f->DeletePending = del;
        return STATUS_SUCCESS;
    }
    case FileRenameInformation: {
        const char *target = s->Parameters.SetFile.TargetName;
        if (!target || !fat_parent(n))
            return STATUS_INVALID_PARAMETER;
        char parent_path[512];
        const char *leaf;
        split_path(target, parent_path, sizeof(parent_path), &leaf);
        if (!*leaf)
            return STATUS_OBJECT_NAME_INVALID;
        fat_node *dst;
        int e = fat_open(v->Fat, parent_path, &dst);
        if (e)
            return e == FAT_ENOENT ? STATUS_OBJECT_PATH_NOT_FOUND : status_of(e);
        fat_info fi;
        fat_stat(n, &fi);
        fat_node *src = fat_parent(n);
        e = fat_rename(src, fi.name, dst, leaf, s->Parameters.SetFile.ReplaceIfExists);
        fat_close(dst);
        if (!e)
            snprintf(f->FileName, sizeof(f->FileName), "\\%s%s%s", parent_path, parent_path[0] ? "\\" : "", leaf);
        return status_of(e);
    }
    default:
        return STATUS_INVALID_PARAMETER;
    }
}

static NTSTATUS query_directory(FILE_OBJECT *f, IO_STACK_LOCATION *s, void *buf, uint64_t *info)
{
    fat_node *n = f->FsContext;
    CCB *ccb = f->FsContext2;
    if (!n || !fat_is_dir(n))
        return STATUS_INVALID_PARAMETER;
    uint32_t flags = s->Parameters.QueryDirectory.Flags;
    if (flags & SL_RESTART_SCAN)
        ccb->Cookie = 0;
    if (s->Parameters.QueryDirectory.FileName && s->Parameters.QueryDirectory.FileName[0])
        strlcpy(ccb->Pattern, s->Parameters.QueryDirectory.FileName, sizeof(ccb->Pattern));
    uint32_t len = s->Parameters.QueryDirectory.Length;
    uint8_t *out = buf;
    uint32_t used = 0;
    FILE_DIRECTORY_INFORMATION *last = NULL;
    fat_info *fi = ExAllocatePoolWithTag(NonPagedPool, sizeof(fat_info), 'FatX');
    if (!fi)
        return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS st = STATUS_SUCCESS;
    for (;;) {
        uint32_t before = ccb->Cookie;
        int r = fat_readdir(n, &ccb->Cookie, fi);
        if (r < 0) {
            st = status_of(r);
            break;
        }
        if (r == 0)
            break;
        if (!wild_match(ccb->Pattern, fi->name) && !wild_match(ccb->Pattern, fi->short_name))
            continue;
        uint32_t nl = (uint32_t)strlen(fi->name);
        uint32_t need = (uint32_t)ALIGN_UP(offsetof(FILE_DIRECTORY_INFORMATION, FileName) + nl + 1, 8);
        if (used + need > len) {
            ccb->Cookie = before;               /* return it next time */
            if (!used)
                st = STATUS_BUFFER_OVERFLOW;
            break;
        }
        FILE_DIRECTORY_INFORMATION *d = (FILE_DIRECTORY_INFORMATION *)(out + used);
        memset(d, 0, need);
        d->FileIndex = before;
        d->CreationTime = nt_time(&fi->created);
        d->LastAccessTime = nt_time(&fi->accessed);
        d->LastWriteTime = d->ChangeTime = nt_time(&fi->modified);
        d->EndOfFile = fi->size;
        d->AllocationSize = fi->size;
        d->FileAttributes = fi->attr ? fi->attr : FILE_ATTRIBUTE_NORMAL;
        strlcpy(d->ShortName, fi->short_name, sizeof(d->ShortName));
        d->FileNameLength = nl;
        memcpy(d->FileName, fi->name, nl + 1);
        if (last)
            last->NextEntryOffset = (uint32_t)((uint8_t *)d - (uint8_t *)last);
        last = d;
        used += need;
        if (flags & SL_RETURN_SINGLE_ENTRY)
            break;
    }
    ExFreePoolWithTag(fi, 'FatX');
    if (NT_SUCCESS(st) && !used)
        st = STATUS_NO_MORE_FILES;
    *info = used;
    return st;
}

static NTSTATUS query_volume(VCB *v, IO_STACK_LOCATION *s, void *buf, uint64_t *info)
{
    uint32_t len = s->Parameters.QueryVolume.Length;
    fat_volinfo vi;
    fat_volume_info(v->Fat, &vi);
    switch (s->Parameters.QueryVolume.FsInformationClass) {
    case FileFsVolumeInformation: {
        if (len < sizeof(FILE_FS_VOLUME_INFORMATION))
            return STATUS_BUFFER_TOO_SMALL;
        FILE_FS_VOLUME_INFORMATION *vol = buf;
        memset(vol, 0, sizeof(*vol));
        vol->VolumeSerialNumber = vi.serial;
        vol->VolumeLabelLength = (uint32_t)strlen(vi.label);
        strlcpy(vol->VolumeLabel, vi.label, sizeof(vol->VolumeLabel));
        *info = sizeof(*vol);
        return STATUS_SUCCESS;
    }
    case FileFsSizeInformation: {
        if (len < sizeof(FILE_FS_SIZE_INFORMATION))
            return STATUS_BUFFER_TOO_SMALL;
        FILE_FS_SIZE_INFORMATION *sz = buf;
        sz->TotalAllocationUnits = vi.total_clusters;
        sz->AvailableAllocationUnits = vi.free_clusters;
        sz->SectorsPerAllocationUnit = vi.sectors_per_cluster;
        sz->BytesPerSector = vi.bytes_per_sector;
        *info = sizeof(*sz);
        return STATUS_SUCCESS;
    }
    case FileFsAttributeInformation: {
        if (len < sizeof(FILE_FS_ATTRIBUTE_INFORMATION))
            return STATUS_BUFFER_TOO_SMALL;
        FILE_FS_ATTRIBUTE_INFORMATION *a = buf;
        a->FileSystemAttributes = 0x2 | 0x4;      /* case preserved, Unicode names on disk */
        a->MaximumComponentNameLength = 255;
        strlcpy(a->FileSystemName, v->FsName, sizeof(a->FileSystemName));
        a->FileSystemNameLength = (uint32_t)strlen(a->FileSystemName);
        *info = sizeof(*a);
        return STATUS_SUCCESS;
    }
    default:
        return STATUS_INVALID_PARAMETER;
    }
}

/* ---- Dispatch ------------------------------------------------------------------------ */

static NTSTATUS fsd_dispatch(DEVICE_OBJECT *dev, IRP *irp)
{
    if (dev == g_cdo)
        return complete(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    VCB *v = dev->DeviceExtension;
    IO_STACK_LOCATION *s = IoGetCurrentIrpStackLocation(irp);
    FILE_OBJECT *f = s->FileObject;
    uint64_t info = 0;
    NTSTATUS st = STATUS_SUCCESS;
    bool last_close = false;

    KeWaitForSingleObject(&v->Lock, KE_INFINITE);
    if (v->Dismounted && s->MajorFunction != IRP_MJ_CLOSE && s->MajorFunction != IRP_MJ_CLEANUP) {
        KeReleaseMutex(&v->Lock);
        return complete(irp, STATUS_VOLUME_DISMOUNTED, 0);
    }
    fat_node *n = f ? f->FsContext : NULL;
    switch (s->MajorFunction) {
    case IRP_MJ_CREATE:
        st = do_create(v, irp, &info);
        if (NT_SUCCESS(st))
            v->OpenFiles++;
        break;
    case IRP_MJ_READ: {
        if (!n) { st = STATUS_INVALID_DEVICE_REQUEST; break; }
        if (fat_is_dir(n)) { st = STATUS_INVALID_DEVICE_REQUEST; break; }
        uint32_t done = 0;
        fat_info fi;
        fat_stat(n, &fi);
        if (s->Parameters.Read.ByteOffset >= fi.size && s->Parameters.Read.Length) {
            st = STATUS_END_OF_FILE;
            break;
        }
        st = status_of(fat_read(n, s->Parameters.Read.ByteOffset, irp->SystemBuffer, s->Parameters.Read.Length, &done));
        info = done;
        break;
    }
    case IRP_MJ_WRITE: {
        if (!n || fat_is_dir(n)) { st = STATUS_INVALID_DEVICE_REQUEST; break; }
        if (!wants_write(f->DesiredAccess)) { st = STATUS_ACCESS_DENIED; break; }
        uint64_t off = s->Parameters.Write.ByteOffset;
        if (off == UINT64_MAX) {                 /* append */
            fat_info fi;
            fat_stat(n, &fi);
            off = fi.size;
        }
        uint32_t done = 0;
        st = status_of(fat_write(n, off, irp->SystemBuffer, s->Parameters.Write.Length, &done));
        info = done;
        break;
    }
    case IRP_MJ_QUERY_INFORMATION:
        st = query_info(v, f, s->Parameters.QueryFile.FileInformationClass, irp->SystemBuffer,
                        s->Parameters.QueryFile.Length, &info);
        break;
    case IRP_MJ_SET_INFORMATION:
        st = set_info(v, f, s, irp->SystemBuffer);
        break;
    case IRP_MJ_DIRECTORY_CONTROL:
        st = s->MinorFunction == IRP_MN_QUERY_DIRECTORY ? query_directory(f, s, irp->SystemBuffer, &info)
                                                         : STATUS_INVALID_DEVICE_REQUEST;
        break;
    case IRP_MJ_QUERY_VOLUME_INFORMATION:
        st = query_volume(v, s, irp->SystemBuffer, &info);
        break;
    case IRP_MJ_FLUSH_BUFFERS:
        if (n)
            fat_flush(n);
        st = status_of(fat_sync(v->Fat));
        if (NT_SUCCESS(st))
            st = IoSynchronousFlush(v->Real);
        break;
    case IRP_MJ_CLEANUP: {
        CCB *ccb = f->FsContext2;
        if (ccb && ccb->DeleteOnClose && n && fat_parent(n) && v->Fat) {
            int e = fat_remove(n);
            if (e)
                DbgPrint("FastFat: delete of %s failed: %s", f->FileName, fat_strerror(e));
        }
        if (n && v->Fat)
            fat_flush(n);
        /* Removable media: once nothing is open, mark the volume clean, so a
           disk pulled out now is not left "dirty". */
        if (v->Fat && v->OpenFiles <= 1 && (v->Real->Characteristics & FILE_REMOVABLE_MEDIA))
            fat_mark_clean(v->Fat);
        break;
    }
    case IRP_MJ_CLOSE:
        if (n && v->Fat)
            fat_close(n);
        if (f->FsContext2)
            ExFreePoolWithTag(f->FsContext2, 'FatX');
        f->FsContext = f->FsContext2 = NULL;
        v->OpenFiles--;
        if (v->Dismounted && v->OpenFiles == 0) {
            teardown(v);                    /* no-op if a forced dismount did it already */
            last_close = true;
        }
        break;
    default:
        st = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }
    KeReleaseMutex(&v->Lock);
    complete(irp, st, info);
    if (last_close) {
        if (v->Vpb->DeviceObject == dev)
            v->Vpb->DeviceObject = NULL;
        IoDeleteDevice(dev);
    }
    return st;
}

static NTSTATUS fastfat_entry(DRIVER_OBJECT *d)
{
    g_driver = d;
    for (int i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        d->MajorFunction[i] = fsd_dispatch;
    d->MajorFunction[IRP_MJ_FILE_SYSTEM_CONTROL] = fsd_fs_control;
    NTSTATUS st = IoCreateDevice(d, 0, "\\Device\\FastFat", FILE_DEVICE_DISK_FILE_SYSTEM, 0, &g_cdo);
    if (!NT_SUCCESS(st))
        return st;
    return IoRegisterFileSystem(g_cdo);
}

void FastFatInitialize(void)
{
    IoCreateDriver("fastfat", fastfat_entry);
}
