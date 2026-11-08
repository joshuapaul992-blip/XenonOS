/* SPDX-License-Identifier: MIT */
/*
 * Io - driver objects, device objects and IRP processing.
 */
#include "io.h"
#include "mm.h"
#include "rtl.h"
#include "console.h"

OBJECT_TYPE *IoDriverObjectType, *IoDeviceObjectType;
void IopInitFileSystem(void);

static NTSTATUS invalid_request(DEVICE_OBJECT *dev, IRP *irp)
{
    UNUSED(dev);
    irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp);
    return STATUS_INVALID_DEVICE_REQUEST;
}

void IoInitSystem(void)
{
    if (!NT_SUCCESS(ObCreateObjectType("Driver", NULL, 'IoDr', &IoDriverObjectType)) ||
        !NT_SUCCESS(ObCreateObjectType("Device", NULL, 'IoDv', &IoDeviceObjectType)))
        KeBugCheckEx(PHASE1_INITIALIZATION_FAILED, 0x10, 0, 0, 0);
    IoDeviceObjectType->ParseObjects = true;    /* paths continue into file systems */
    IopInitFileSystem();
}

NTSTATUS IoCreateDriver(const char *name, NTSTATUS (*entry)(DRIVER_OBJECT *driver))
{
    DRIVER_OBJECT *d;
    NTSTATUS st = ObCreateObject(IoDriverObjectType, sizeof(DRIVER_OBJECT), (void **)&d);
    if (!NT_SUCCESS(st))
        return st;
    strlcpy(d->DriverName, name, sizeof(d->DriverName));
    for (int i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        d->MajorFunction[i] = invalid_request;
    char path[64];
    snprintf(path, sizeof(path), "\\Driver\\%s", name);
    st = ObInsertObjectName(d, path, OBJ_PERMANENT);
    if (!NT_SUCCESS(st)) {
        ObDereferenceObject(d);
        return st;
    }
    ObDereferenceObject(d);         /* the namespace keeps it */
    st = entry(d);
    if (!NT_SUCCESS(st))
        DbgPrint("Io: driver %s failed to start: %s", name, RtlStatusName(st));
    return st;
}

NTSTATUS IoCreateDevice(DRIVER_OBJECT *driver, size_t extension_size, const char *name, uint32_t type,
                        uint32_t characteristics, DEVICE_OBJECT **device)
{
    DEVICE_OBJECT *d;
    size_t ext = ALIGN_UP(extension_size, 16);
    NTSTATUS st = ObCreateObject(IoDeviceObjectType, sizeof(DEVICE_OBJECT) + ext, (void **)&d);
    if (!NT_SUCCESS(st))
        return st;
    d->DriverObject = driver;
    d->DeviceType = type;
    d->Characteristics = characteristics;
    d->StackSize = 1;
    d->SectorSize = 512;
    d->DeviceExtension = ext ? (void *)(d + 1) : NULL;
    if (name) {
        st = ObInsertObjectName(d, name, OBJ_PERMANENT);
        if (!NT_SUCCESS(st)) {
            ObDereferenceObject(d);
            return st;
        }
        ObDereferenceObject(d);     /* the namespace keeps it */
    }
    d->NextDevice = driver->DeviceObject;
    driver->DeviceObject = d;
    *device = d;
    return STATUS_SUCCESS;
}

void IoDeleteDevice(DEVICE_OBJECT *device)
{
    DRIVER_OBJECT *drv = device->DriverObject;
    for (DEVICE_OBJECT **pp = &drv->DeviceObject; *pp; pp = &(*pp)->NextDevice) {
        if (*pp == device) {
            *pp = device->NextDevice;
            break;
        }
    }
    if (device->AttachedTo)
        device->AttachedTo->AttachedDevice = NULL;
    if (ObGetObjectName(device)[0]) {
        ObMakeTemporaryObject(device);     /* drops the name and the namespace's reference */
    } else {
        ObDereferenceObject(device);
    }
}

DEVICE_OBJECT *IoGetAttachedDevice(DEVICE_OBJECT *device)
{
    while (device->AttachedDevice)
        device = device->AttachedDevice;
    return device;
}

DEVICE_OBJECT *IoAttachDeviceToDeviceStack(DEVICE_OBJECT *source, DEVICE_OBJECT *target)
{
    DEVICE_OBJECT *top = IoGetAttachedDevice(target);
    top->AttachedDevice = source;
    source->AttachedTo = top;
    source->StackSize = (int8_t)(top->StackSize + 1);
    source->SectorSize = top->SectorSize;
    return top;
}

NTSTATUS IoCreateSymbolicLink(const char *link, const char *target)
{
    return ObCreateSymbolicLink(link, target);
}

NTSTATUS IoDeleteSymbolicLink(const char *link)
{
    void *o;
    NTSTATUS st = ObReferenceObjectByName(link, OBJ_OPENLINK, ObSymbolicLinkObjectType, &o);
    if (!NT_SUCCESS(st))
        return st;
    ObMakeTemporaryObject(o);
    ObDereferenceObject(o);
    return STATUS_SUCCESS;
}

NTSTATUS IoGetDeviceObjectPointer(const char *name, DEVICE_OBJECT **device)
{
    char path[160];
    if (name[0] == '\\' || name[0] == '/')
        strlcpy(path, name, sizeof(path));
    else if (!strncasecmp(name, "//./", 4) || !strncasecmp(name, "\\\\.\\", 4))
        snprintf(path, sizeof(path), "\\GLOBAL??\\%s", name + 4);
    else
        snprintf(path, sizeof(path), "\\GLOBAL??\\%s", name);
    void *o;
    NTSTATUS st = ObReferenceObjectByName(path, 0, IoDeviceObjectType, &o);
    if (st == STATUS_OBJECT_NAME_NOT_FOUND && name[0] != '\\' && name[0] != '/') {
        snprintf(path, sizeof(path), "\\Device\\%s", name);
        st = ObReferenceObjectByName(path, 0, IoDeviceObjectType, &o);
    }
    if (NT_SUCCESS(st))
        *device = o;
    return st;
}

/* ---- IRPs --------------------------------------------------------------- */

IRP *IoAllocateIrp(int8_t stack_size)
{
    if (stack_size < 1)
        stack_size = 1;
    IRP *irp = ExAllocatePoolWithTag(NonPagedPoolZeroed, sizeof(IRP) + stack_size * sizeof(IO_STACK_LOCATION), 'IoIr');
    if (!irp)
        return NULL;
    irp->StackCount = stack_size;
    irp->CurrentLocation = stack_size;      /* "above" the stack: no current location yet */
    return irp;
}

void IoFreeIrp(IRP *irp)
{
    ExFreePoolWithTag(irp, 'IoIr');
}

NTSTATUS IoCallDriver(DEVICE_OBJECT *device, IRP *irp)
{
    if (irp->CurrentLocation <= 0)
        KeBugCheckEx(0x35 /* NO_MORE_IRP_STACK_LOCATIONS */, (uint64_t)irp, (uint64_t)device, 0, 0);
    irp->CurrentLocation--;
    IO_STACK_LOCATION *s = IoGetCurrentIrpStackLocation(irp);
    s->DeviceObject = device;
    return device->DriverObject->MajorFunction[s->MajorFunction](device, irp);
}

void IoCopyCurrentIrpStackLocationToNext(IRP *irp)
{
    IO_STACK_LOCATION *cur = IoGetCurrentIrpStackLocation(irp), *next = IoGetNextIrpStackLocation(irp);
    *next = *cur;
    next->CompletionRoutine = NULL;
    next->Context = NULL;
}

void IoSetCompletionRoutine(IRP *irp, PIO_COMPLETION_ROUTINE routine, void *context)
{
    IO_STACK_LOCATION *next = IoGetNextIrpStackLocation(irp);
    next->CompletionRoutine = routine;
    next->Context = context;
}

void IoCompleteRequest(IRP *irp)
{
    for (int loc = irp->CurrentLocation; loc < irp->StackCount; loc++) {
        IO_STACK_LOCATION *s = &irp->Stack[loc];
        irp->CurrentLocation = (int8_t)(loc + 1);
        if (s->CompletionRoutine) {
            DEVICE_OBJECT *upper = loc + 1 < irp->StackCount ? irp->Stack[loc + 1].DeviceObject : NULL;
            NTSTATUS st = s->CompletionRoutine(upper, irp, s->Context);
            if (st == STATUS_MORE_PROCESSING_REQUIRED)
                return;             /* the routine owns the IRP now */
        }
    }
    if (irp->UserIosb)
        *irp->UserIosb = irp->IoStatus;
    KEVENT *ev = irp->UserEvent;
    if (irp->Flags & IRP_FREE_ON_COMPLETE)
        IoFreeIrp(irp);
    if (ev)
        KeSetEvent(ev);
}

NTSTATUS IopSendAndWait(DEVICE_OBJECT *device, IRP *irp, uint64_t *information)
{
    KEVENT ev;
    IO_STATUS_BLOCK iosb = { STATUS_PENDING, 0 };
    KeInitializeEvent(&ev, NotificationEvent, false);
    irp->UserEvent = &ev;
    irp->UserIosb = &iosb;
    irp->Flags |= IRP_FREE_ON_COMPLETE;
    NTSTATUS st = IoCallDriver(device, irp);
    if (st == STATUS_PENDING) {
        KeWaitForSingleObject(&ev, KE_INFINITE);
    } else {
        /* Completed synchronously: the event is already set. */
        KeWaitForSingleObject(&ev, KE_INFINITE);
    }
    if (information)
        *information = iosb.Information;
    return iosb.Status;
}

NTSTATUS IoSynchronousReadWrite(DEVICE_OBJECT *device, bool write, void *buffer, uint32_t length,
                                uint64_t offset, uint64_t *transferred)
{
    device = IoGetAttachedDevice(device);
    IRP *irp = IoAllocateIrp(device->StackSize);
    if (!irp)
        return STATUS_INSUFFICIENT_RESOURCES;
    irp->SystemBuffer = buffer;
    IO_STACK_LOCATION *s = IoGetNextIrpStackLocation(irp);
    s->MajorFunction = write ? IRP_MJ_WRITE : IRP_MJ_READ;
    s->Parameters.Read.Length = length;
    s->Parameters.Read.ByteOffset = offset;
    return IopSendAndWait(device, irp, transferred);
}

NTSTATUS IoSynchronousDeviceControl(DEVICE_OBJECT *device, uint32_t code, void *in, uint32_t in_len,
                                    void *out, uint32_t out_len, uint64_t *returned)
{
    device = IoGetAttachedDevice(device);
    IRP *irp = IoAllocateIrp(device->StackSize);
    if (!irp)
        return STATUS_INSUFFICIENT_RESOURCES;
    /* METHOD_BUFFERED: one buffer, input copied in, output copied out. */
    uint32_t len = MAX(in_len, out_len);
    void *buf = len ? ExAllocatePoolWithTag(NonPagedPoolZeroed, len, 'IoBu') : NULL;
    if (len && !buf) {
        IoFreeIrp(irp);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (in_len)
        memcpy(buf, in, in_len);
    irp->SystemBuffer = buf;
    IO_STACK_LOCATION *s = IoGetNextIrpStackLocation(irp);
    s->MajorFunction = IRP_MJ_DEVICE_CONTROL;
    s->Parameters.DeviceIoControl.IoControlCode = code;
    s->Parameters.DeviceIoControl.InputBufferLength = in_len;
    s->Parameters.DeviceIoControl.OutputBufferLength = out_len;
    uint64_t info = 0;
    NTSTATUS st = IopSendAndWait(device, irp, &info);
    if (NT_SUCCESS(st) && out_len)
        memcpy(out, buf, MIN(info, out_len));
    if (returned)
        *returned = info;
    if (buf)
        ExFreePoolWithTag(buf, 'IoBu');
    return st;
}

NTSTATUS IoSynchronousFlush(DEVICE_OBJECT *device)
{
    device = IoGetAttachedDevice(device);
    IRP *irp = IoAllocateIrp(device->StackSize);
    if (!irp)
        return STATUS_INSUFFICIENT_RESOURCES;
    IoGetNextIrpStackLocation(irp)->MajorFunction = IRP_MJ_FLUSH_BUFFERS;
    return IopSendAndWait(device, irp, NULL);
}

void IoShutdownSystem(void)
{
    MountMgrShutdown();
    NTSTATUS st = CcFlushAll();
    for (unsigned i = 0; i < DiskCount(); i++)
        IoSynchronousFlush(DiskGet(i));
    DbgPrint("Io: disk caches flushed for shutdown (%s)", NT_SUCCESS(st) ? "ok" : RtlStatusName(st));
}
