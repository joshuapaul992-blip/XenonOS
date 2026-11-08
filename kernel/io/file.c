/* SPDX-License-Identifier: MIT */
/*
 * Io - file objects, mounting, and the native file API.
 *
 * Opening "C:\XenonOS\Config\hostname":
 *   1. RtlDosPathToNtPath gives "\??\C:\XenonOS\Config\hostname".
 *   2. The object manager follows \?? -> \GLOBAL?? and C: ->
 *      \Device\HarddiskVolume1, stops at that device (devices are "parse"
 *      objects) and returns the rest: "XenonOS\Config\hostname".
 *   3. If no file system is mounted on the volume yet, or its medium was
 *      changed, the I/O manager mounts one: it offers the volume to each
 *      registered file system (IRP_MN_MOUNT_VOLUME) until one accepts.
 *   4. A file object is created and IRP_MJ_CREATE goes to the file system's
 *      volume device with the remaining path.
 *
 * Devices that are not volumes (a whole disk, \Device\Harddisk0\DR0) open
 * "raw": reads and writes go straight to the device.
 */
#include "io.h"
#include "ps.h"
#include "mm.h"
#include "hal.h"
#include "rtl.h"
#include "console.h"

NTSTATUS IopSendAndWait(DEVICE_OBJECT *device, IRP *irp, uint64_t *information);

OBJECT_TYPE *IoFileObjectType;

#define MAX_FILE_SYSTEMS 8
static DEVICE_OBJECT *g_fs[MAX_FILE_SYSTEMS];
static unsigned g_fs_count;
static KMUTEX g_mount_lock;

#define FO_OPENED 0x8000          /* IRP_MJ_CREATE succeeded: send CLEANUP/CLOSE */

/* ---- Time ------------------------------------------------------------------ */

#define UNIX_EPOCH_AS_NT 116444736000000000ll
static int64_t g_boot_unix;
static bool g_have_time;

int64_t RtlUnixToNtTime(int64_t unix_seconds) { return unix_seconds * 10000000ll + UNIX_EPOCH_AS_NT; }
int64_t RtlNtTimeToUnix(int64_t nt) { return (nt - UNIX_EPOCH_AS_NT) / 10000000ll; }

int64_t KeQuerySystemTime(void)
{
    if (!g_have_time) {
        TIME_FIELDS t;
        g_boot_unix = HalQueryRealTimeClock(&t) ? RtlTimeFieldsToUnix(&t) - (int64_t)(KeQueryUptimeNs() / 1000000000ull)
                                                : 1767225600;   /* 2026-01-01 if there is no RTC */
        g_have_time = true;
    }
    uint64_t ns = KeQueryUptimeNs();
    return RtlUnixToNtTime(g_boot_unix) + (int64_t)(ns / 100);
}

/* ---- Paths ------------------------------------------------------------------- */

NTSTATUS RtlDosPathToNtPath(const char *dos, char *nt, size_t size)
{
    size_t n = 0;
    const char *p = dos;
    if ((p[0] == '\\' || p[0] == '/') && (p[1] == '\\' || p[1] == '/') && p[2] == '.' && (p[3] == '\\' || p[3] == '/')) {
        n = (size_t)snprintf(nt, size, "\\??\\");
        p += 4;
    } else if (p[0] == '\\' && p[1] != '\\') {
        n = 0;                                  /* already an NT path */
    } else if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':') {
        n = (size_t)snprintf(nt, size, "\\??\\%c:", toupper(p[0]));
        p += 2;
        if (*p && *p != '/' && *p != '\\') {
            if (n + 1 >= size) return STATUS_NAME_TOO_LONG;
            nt[n++] = '\\';                     /* "C:foo" is treated as "C:\foo" */
        }
    } else {
        return STATUS_OBJECT_PATH_NOT_FOUND;    /* relative paths are the caller's job */
    }
    for (; *p; p++) {
        if (n + 1 >= size)
            return STATUS_NAME_TOO_LONG;
        char c = *p == '/' ? '\\' : *p;
        if (c == '\\' && n && nt[n - 1] == '\\')
            continue;                           /* collapse "//" */
        nt[n++] = c;
    }
    if (n == 0 || (n >= 2 && nt[n - 1] == ':')) {
        if (n + 1 >= size) return STATUS_NAME_TOO_LONG;
        nt[n++] = '\\';                         /* "C:" means the root of C: */
    }
    nt[n] = 0;
    return STATUS_SUCCESS;
}

/* ---- Volumes and mounting --------------------------------------------------- */

void IoMarkVolumeDevice(DEVICE_OBJECT *dev)
{
    if (dev->Vpb)
        return;
    VPB *v = ExAllocatePoolWithTag(NonPagedPoolZeroed, sizeof(VPB), 'IoVp');
    if (!v)
        return;
    v->RealDevice = dev;
    dev->Vpb = v;
}

bool IoIsVolumeDevice(DEVICE_OBJECT *dev)
{
    return dev->Vpb != NULL;
}

NTSTATUS IoRegisterFileSystem(DEVICE_OBJECT *fs)
{
    if (g_fs_count >= MAX_FILE_SYSTEMS)
        return STATUS_INSUFFICIENT_RESOURCES;
    g_fs[g_fs_count++] = fs;
    return STATUS_SUCCESS;
}

NTSTATUS IoQueryMediaChangeCount(DEVICE_OBJECT *dev, uint32_t *count)
{
    XENON_DISK_INFO info;
    NTSTATUS st = IoSynchronousDeviceControl(dev, IOCTL_XENON_QUERY_DISK, NULL, 0, &info, sizeof(info), NULL);
    if (NT_SUCCESS(st)) {
        *count = info.MediaChangeCount;
        if (!info.MediaPresent)
            st = STATUS_NO_MEDIA_IN_DEVICE;
    }
    return st;
}

static NTSTATUS fs_control(DEVICE_OBJECT *target, uint8_t minor, VPB *vpb, DEVICE_OBJECT *real, uint32_t code)
{
    IRP *irp = IoAllocateIrp(target->StackSize);
    if (!irp)
        return STATUS_INSUFFICIENT_RESOURCES;
    IO_STACK_LOCATION *s = IoGetNextIrpStackLocation(irp);
    s->MajorFunction = IRP_MJ_FILE_SYSTEM_CONTROL;
    s->MinorFunction = minor;
    s->Parameters.FileSystemControl.Vpb = vpb;
    s->Parameters.FileSystemControl.DeviceObject = real;
    s->Parameters.FileSystemControl.FsControlCode = code;
    return IopSendAndWait(target, irp, NULL);
}

enum { DISMOUNT_NORMAL, DISMOUNT_FORCE, DISMOUNT_INVALIDATE };

static NTSTATUS dismount_locked(VPB *vpb, int how)
{
    if (!(vpb->Flags & VPB_MOUNTED))
        return STATUS_SUCCESS;
    bool force = how != DISMOUNT_NORMAL;
    if (vpb->ReferenceCount && !force)
        return STATUS_SHARING_VIOLATION;
    uint32_t code = how == DISMOUNT_NORMAL ? FSCTL_DISMOUNT_VOLUME
                  : how == DISMOUNT_FORCE ? FSCTL_XENON_FORCE_DISMOUNT : FSCTL_XENON_INVALIDATE;
    NTSTATUS st = fs_control(vpb->DeviceObject, IRP_MN_USER_FS_REQUEST, vpb, vpb->RealDevice, code);
    if (NT_SUCCESS(st) || force)
        vpb->Flags &= ~VPB_MOUNTED;
    return st;
}

NTSTATUS IoDismountVolume(DEVICE_OBJECT *volume)
{
    if (!volume->Vpb)
        return STATUS_INVALID_DEVICE_REQUEST;
    KeWaitForSingleObject(&g_mount_lock, KE_INFINITE);
    NTSTATUS st = dismount_locked(volume->Vpb, DISMOUNT_NORMAL);
    KeReleaseMutex(&g_mount_lock);
    return st;
}

NTSTATUS IoForceDismountVolume(DEVICE_OBJECT *volume)
{
    if (!volume->Vpb)
        return STATUS_INVALID_DEVICE_REQUEST;
    KeWaitForSingleObject(&g_mount_lock, KE_INFINITE);
    NTSTATUS st = dismount_locked(volume->Vpb, DISMOUNT_FORCE);
    KeReleaseMutex(&g_mount_lock);
    return st;
}

NTSTATUS IoMountVolume(DEVICE_OBJECT *volume)
{
    VPB *vpb = volume->Vpb;
    if (!vpb)
        return STATUS_INVALID_DEVICE_REQUEST;
    KeWaitForSingleObject(&g_mount_lock, KE_INFINITE);
    uint32_t count = 0;
    NTSTATUS st = IoQueryMediaChangeCount(volume, &count);
    if (st == STATUS_NO_MEDIA_IN_DEVICE) {
        if (vpb->Flags & VPB_MOUNTED)
            dismount_locked(vpb, DISMOUNT_INVALIDATE);   /* medium is gone: drop the volume */
        KeReleaseMutex(&g_mount_lock);
        return st;
    }
    if ((vpb->Flags & VPB_MOUNTED) && NT_SUCCESS(st) && count != vpb->MediaChangeCount) {
        DbgPrint("Io: medium in %s changed - remounting", ObGetObjectName(volume));
        dismount_locked(vpb, DISMOUNT_INVALIDATE);       /* never write to the new medium */
    }
    if (vpb->Flags & VPB_MOUNTED) {
        KeReleaseMutex(&g_mount_lock);
        return STATUS_SUCCESS;
    }
    st = STATUS_UNRECOGNIZED_VOLUME;
    for (unsigned i = 0; i < g_fs_count; i++) {
        st = fs_control(g_fs[i], IRP_MN_MOUNT_VOLUME, vpb, volume, 0);
        if (NT_SUCCESS(st)) {
            vpb->Flags |= VPB_MOUNTED;
            vpb->MediaChangeCount = count;
            break;
        }
    }
    KeReleaseMutex(&g_mount_lock);
    return st;
}

/* ---- File objects ------------------------------------------------------------- */

static DEVICE_OBJECT *fs_device(FILE_OBJECT *f)
{
    return f->FsDevice;
}

static NTSTATUS call_fs(FILE_OBJECT *f, uint8_t major, uint8_t minor, const IO_STACK_LOCATION *params,
                        void *buffer, uint64_t *info)
{
    DEVICE_OBJECT *dev = fs_device(f);
    if (!dev)
        return STATUS_INVALID_DEVICE_REQUEST;
    IRP *irp = IoAllocateIrp(dev->StackSize);
    if (!irp)
        return STATUS_INSUFFICIENT_RESOURCES;
    irp->SystemBuffer = buffer;
    IO_STACK_LOCATION *s = IoGetNextIrpStackLocation(irp);
    if (params)
        s->Parameters = params->Parameters;
    s->MajorFunction = major;
    s->MinorFunction = minor;
    s->FileObject = f;
    return IopSendAndWait(dev, irp, info);
}

static void file_close_procedure(void *object)
{
    FILE_OBJECT *f = object;
    if ((f->Flags & FO_OPENED) && f->Vpb)
        call_fs(f, IRP_MJ_CLEANUP, 0, NULL, NULL, NULL);
}

static void file_delete_procedure(void *object)
{
    FILE_OBJECT *f = object;
    if ((f->Flags & FO_OPENED) && f->Vpb) {
        call_fs(f, IRP_MJ_CLOSE, 0, NULL, NULL, NULL);
        __atomic_sub_fetch(&f->Vpb->ReferenceCount, 1, __ATOMIC_RELAXED);
    }
    if (f->DeviceObject)
        ObDereferenceObject(f->DeviceObject);
}

void IopInitFileSystem(void)
{
    KeInitializeMutex(&g_mount_lock);
    if (!NT_SUCCESS(ObCreateObjectType("File", file_delete_procedure, 'IoFi', &IoFileObjectType)))
        KeBugCheckEx(PHASE1_INITIALIZATION_FAILED, 0x11, 0, 0, 0);
    IoFileObjectType->CloseProcedure = file_close_procedure;
}

/* Resolve a path to a device and the path inside it. */
static NTSTATUS resolve(const char *path, DEVICE_OBJECT **dev, char *rest, size_t rest_size)
{
    char nt[600];
    NTSTATUS st = RtlDosPathToNtPath(path, nt, sizeof(nt));
    if (!NT_SUCCESS(st))
        return st;
    void *o;
    st = ObReferenceObjectByNameEx(nt, 0, &o, rest, rest_size);
    if (!NT_SUCCESS(st))
        return st;
    if (ObGetObjectType(o) != IoDeviceObjectType) {
        ObDereferenceObject(o);
        return STATUS_OBJECT_TYPE_MISMATCH;
    }
    *dev = o;
    return STATUS_SUCCESS;
}

NTSTATUS NtCreateFile(HANDLE *handle, uint32_t access, const char *path, IO_STATUS_BLOCK *iosb,
                      uint32_t attributes, uint32_t disposition, uint32_t options)
{
    DEVICE_OBJECT *dev;
    char rest[512];
    NTSTATUS st = resolve(path, &dev, rest, sizeof(rest));
    if (!NT_SUCCESS(st))
        return st;
    FILE_OBJECT *f;
    st = ObCreateObject(IoFileObjectType, sizeof(FILE_OBJECT), (void **)&f);
    if (!NT_SUCCESS(st)) {
        ObDereferenceObject(dev);
        return st;
    }
    f->DeviceObject = dev;                 /* the file object keeps the device reference */
    f->DesiredAccess = access;
    uint64_t info = FILE_OPENED;

    if (!IoIsVolumeDevice(dev)) {
        /* A device with no file system: whole disks and the like. */
        if (rest[0]) {
            st = STATUS_OBJECT_PATH_NOT_FOUND;
            goto fail;
        }
        f->Flags |= FO_RAW_DEVICE;
    } else {
        st = IoMountVolume(dev);
        if (!NT_SUCCESS(st))
            goto fail;
        f->Vpb = dev->Vpb;
        f->FsDevice = dev->Vpb->DeviceObject;
        __atomic_add_fetch(&f->Vpb->ReferenceCount, 1, __ATOMIC_RELAXED);
        f->Flags |= FO_OPENED;
        if (!rest[0] && !(options & FILE_DIRECTORY_FILE) && disposition == FILE_OPEN && !(access & DELETE))
            f->Flags |= FO_VOLUME_OPEN;         /* "\\.\C:" - the volume itself */
        for (char *c = rest; *c; c++)
            if (*c == '/')
                *c = '\\';
        snprintf(f->FileName, sizeof(f->FileName), "\\%s", rest);
        IO_STACK_LOCATION p = { 0 };
        p.Parameters.Create.Options = (disposition << 24) | (options & 0x00FFFFFF);
        p.Parameters.Create.FileAttributes = attributes;
        p.Parameters.Create.DesiredAccess = access;
        st = call_fs(f, IRP_MJ_CREATE, 0, &p, NULL, &info);
        if (!NT_SUCCESS(st)) {
            f->Flags &= ~FO_OPENED;
            __atomic_sub_fetch(&f->Vpb->ReferenceCount, 1, __ATOMIC_RELAXED);
            goto fail;
        }
    }
    st = ObCreateHandle(f, handle);
    ObDereferenceObject(f);
    if (iosb) {
        iosb->Status = st;
        iosb->Information = info;
    }
    return st;
fail:
    ObDereferenceObject(f);
    if (iosb) {
        iosb->Status = st;
        iosb->Information = 0;
    }
    return st;
}

static NTSTATUS get_file(HANDLE h, FILE_OBJECT **f)
{
    return ObReferenceObjectByHandle(h, IoFileObjectType, (void **)f);
}

static NTSTATUS read_write(HANDLE h, bool write, void *buf, uint32_t len, const int64_t *offset, IO_STATUS_BLOCK *iosb)
{
    FILE_OBJECT *f;
    NTSTATUS st = get_file(h, &f);
    if (!NT_SUCCESS(st))
        return st;
    int64_t off = offset ? *offset : f->CurrentByteOffset;
    uint64_t done = 0;
    if (f->Flags & (FO_RAW_DEVICE | FO_VOLUME_OPEN)) {
        if (off < 0) {
            st = STATUS_INVALID_PARAMETER;
        } else {
            st = IoSynchronousReadWrite(f->DeviceObject, write, buf, len, (uint64_t)off, &done);
        }
    } else {
        IO_STACK_LOCATION p = { 0 };
        p.Parameters.Read.Length = len;
        p.Parameters.Read.ByteOffset = (uint64_t)off;       /* -1 = end of file (append) */
        st = call_fs(f, write ? IRP_MJ_WRITE : IRP_MJ_READ, 0, &p, buf, &done);
        if (off == -1 && NT_SUCCESS(st)) {
            /* Append: the file system reports where the data went in the stack? Keep
               the position at the new end. */
            FILE_STANDARD_INFORMATION si;
            IO_STACK_LOCATION q = { 0 };
            q.Parameters.QueryFile.Length = sizeof(si);
            q.Parameters.QueryFile.FileInformationClass = FileStandardInformation;
            if (NT_SUCCESS(call_fs(f, IRP_MJ_QUERY_INFORMATION, 0, &q, &si, NULL)))
                off = si.EndOfFile - (int64_t)done;
        }
    }
    if (NT_SUCCESS(st) || st == STATUS_END_OF_FILE) {
        if (!offset || off >= 0)
            f->CurrentByteOffset = off + (int64_t)done;
    }
    if (iosb) {
        iosb->Status = st;
        iosb->Information = done;
    }
    ObDereferenceObject(f);
    return st;
}

NTSTATUS NtReadFile(HANDLE h, void *buf, uint32_t len, const int64_t *offset, IO_STATUS_BLOCK *iosb)
{
    return read_write(h, false, buf, len, offset, iosb);
}

NTSTATUS NtWriteFile(HANDLE h, const void *buf, uint32_t len, const int64_t *offset, IO_STATUS_BLOCK *iosb)
{
    return read_write(h, true, (void *)buf, len, offset, iosb);
}

NTSTATUS NtQueryInformationFile(HANDLE h, IO_STATUS_BLOCK *iosb, void *buf, uint32_t len, uint32_t cls)
{
    FILE_OBJECT *f;
    NTSTATUS st = get_file(h, &f);
    if (!NT_SUCCESS(st))
        return st;
    uint64_t info = 0;
    if (cls == FilePositionInformation) {
        if (len < sizeof(FILE_POSITION_INFORMATION)) {
            st = STATUS_BUFFER_TOO_SMALL;
        } else {
            ((FILE_POSITION_INFORMATION *)buf)->CurrentByteOffset = f->CurrentByteOffset;
            info = sizeof(FILE_POSITION_INFORMATION);
        }
    } else if (f->Flags & FO_RAW_DEVICE) {
        st = STATUS_INVALID_DEVICE_REQUEST;
    } else {
        IO_STACK_LOCATION p = { 0 };
        p.Parameters.QueryFile.Length = len;
        p.Parameters.QueryFile.FileInformationClass = cls;
        st = call_fs(f, IRP_MJ_QUERY_INFORMATION, 0, &p, buf, &info);
    }
    if (iosb) {
        iosb->Status = st;
        iosb->Information = info;
    }
    ObDereferenceObject(f);
    return st;
}

NTSTATUS NtSetInformationFile(HANDLE h, IO_STATUS_BLOCK *iosb, void *buf, uint32_t len, uint32_t cls)
{
    FILE_OBJECT *f;
    NTSTATUS st = get_file(h, &f);
    if (!NT_SUCCESS(st))
        return st;
    IO_STACK_LOCATION p = { 0 };
    p.Parameters.SetFile.Length = len;
    p.Parameters.SetFile.FileInformationClass = cls;
    if (cls == FilePositionInformation) {
        if (len < sizeof(FILE_POSITION_INFORMATION) || ((FILE_POSITION_INFORMATION *)buf)->CurrentByteOffset < 0)
            st = STATUS_INVALID_PARAMETER;
        else
            f->CurrentByteOffset = ((FILE_POSITION_INFORMATION *)buf)->CurrentByteOffset;
    } else if (f->Flags & FO_RAW_DEVICE) {
        st = STATUS_INVALID_DEVICE_REQUEST;
    } else if (cls == FileRenameInformation) {
        /* buf: the new name as a path. It must be on the same volume. */
        DEVICE_OBJECT *dev;
        char rest[512];
        st = resolve((const char *)buf, &dev, rest, sizeof(rest));
        if (NT_SUCCESS(st)) {
            if (dev != f->DeviceObject) {
                st = STATUS_NOT_SAME_DEVICE;
            } else {
                for (char *c = rest; *c; c++)
                    if (*c == '/')
                        *c = '\\';
                p.Parameters.SetFile.TargetName = rest;
                p.Parameters.SetFile.ReplaceIfExists = false;
                st = call_fs(f, IRP_MJ_SET_INFORMATION, 0, &p, NULL, NULL);
            }
            ObDereferenceObject(dev);
        }
    } else {
        st = call_fs(f, IRP_MJ_SET_INFORMATION, 0, &p, buf, NULL);
    }
    if (iosb) {
        iosb->Status = st;
        iosb->Information = 0;
    }
    ObDereferenceObject(f);
    return st;
}

NTSTATUS NtQueryDirectoryFile(HANDLE h, IO_STATUS_BLOCK *iosb, void *buf, uint32_t len, uint32_t cls,
                              bool single, const char *pattern, bool restart)
{
    FILE_OBJECT *f;
    NTSTATUS st = get_file(h, &f);
    if (!NT_SUCCESS(st))
        return st;
    IO_STACK_LOCATION p = { 0 };
    p.Parameters.QueryDirectory.Length = len;
    p.Parameters.QueryDirectory.FileInformationClass = cls;
    p.Parameters.QueryDirectory.FileName = pattern;
    p.Parameters.QueryDirectory.Flags = (single ? SL_RETURN_SINGLE_ENTRY : 0) | (restart ? SL_RESTART_SCAN : 0);
    uint64_t info = 0;
    st = (f->Flags & FO_RAW_DEVICE) ? STATUS_INVALID_DEVICE_REQUEST
                                    : call_fs(f, IRP_MJ_DIRECTORY_CONTROL, IRP_MN_QUERY_DIRECTORY, &p, buf, &info);
    if (iosb) {
        iosb->Status = st;
        iosb->Information = info;
    }
    ObDereferenceObject(f);
    return st;
}

NTSTATUS NtQueryVolumeInformationFile(HANDLE h, IO_STATUS_BLOCK *iosb, void *buf, uint32_t len, uint32_t cls)
{
    FILE_OBJECT *f;
    NTSTATUS st = get_file(h, &f);
    if (!NT_SUCCESS(st))
        return st;
    IO_STACK_LOCATION p = { 0 };
    p.Parameters.QueryVolume.Length = len;
    p.Parameters.QueryVolume.FsInformationClass = cls;
    uint64_t info = 0;
    st = (f->Flags & FO_RAW_DEVICE) ? STATUS_INVALID_DEVICE_REQUEST
                                    : call_fs(f, IRP_MJ_QUERY_VOLUME_INFORMATION, 0, &p, buf, &info);
    if (iosb) {
        iosb->Status = st;
        iosb->Information = info;
    }
    ObDereferenceObject(f);
    return st;
}

NTSTATUS NtFlushBuffersFile(HANDLE h, IO_STATUS_BLOCK *iosb)
{
    FILE_OBJECT *f;
    NTSTATUS st = get_file(h, &f);
    if (!NT_SUCCESS(st))
        return st;
    st = (f->Flags & FO_RAW_DEVICE) ? IoSynchronousFlush(f->DeviceObject)
                                    : call_fs(f, IRP_MJ_FLUSH_BUFFERS, 0, NULL, NULL, NULL);
    if (iosb) {
        iosb->Status = st;
        iosb->Information = 0;
    }
    ObDereferenceObject(f);
    return st;
}

NTSTATUS NtDeleteFile(const char *path)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    NTSTATUS st = NtCreateFile(&h, DELETE, path, &iosb, 0, FILE_OPEN, 0);
    if (!NT_SUCCESS(st))
        return st;
    FILE_DISPOSITION_INFORMATION d = { true };
    st = NtSetInformationFile(h, &iosb, &d, sizeof(d), FileDispositionInformation);
    NtClose(h);
    return st;
}

NTSTATUS NtClose(HANDLE h)
{
    return ObCloseHandle(h);
}
