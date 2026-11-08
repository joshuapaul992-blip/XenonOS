/* SPDX-License-Identifier: MIT */
/*
 * Io - I/O manager: driver objects, device objects, IRPs, and the storage
 * stack (disk class, partition manager, block cache).
 *
 * As on Windows NT, a request is an IRP that travels down a stack of
 * devices. Each device in the path gets its own IO_STACK_LOCATION with the
 * parameters meant for it, and can register a completion routine that runs
 * on the way back up.
 */
#pragma once
#include "ke.h"
#include "ob.h"

/* ---- Major function codes (NT values) ---------------------------------- */

#define IRP_MJ_CREATE                   0x00
#define IRP_MJ_CLOSE                    0x02
#define IRP_MJ_READ                     0x03
#define IRP_MJ_WRITE                    0x04
#define IRP_MJ_QUERY_INFORMATION        0x05
#define IRP_MJ_SET_INFORMATION          0x06
#define IRP_MJ_FLUSH_BUFFERS            0x09
#define IRP_MJ_QUERY_VOLUME_INFORMATION 0x0A
#define IRP_MJ_DIRECTORY_CONTROL        0x0C
#define IRP_MJ_FILE_SYSTEM_CONTROL      0x0D
#define IRP_MJ_DEVICE_CONTROL           0x0E
#define IRP_MJ_SHUTDOWN                 0x10
#define IRP_MJ_CLEANUP                  0x12
#define IRP_MJ_PNP                      0x1B
#define IRP_MJ_MAXIMUM_FUNCTION         0x1B

/* ---- Device types and characteristics ---------------------------------- */

#define FILE_DEVICE_CD_ROM              0x02
#define FILE_DEVICE_DISK                0x07
#define FILE_DEVICE_DISK_FILE_SYSTEM    0x08
#define FILE_DEVICE_CONTROLLER          0x04
#define FILE_DEVICE_UNKNOWN             0x22
#define FILE_DEVICE_MASS_STORAGE        0x2D

#define FILE_REMOVABLE_MEDIA            0x00000001
#define FILE_READ_ONLY_DEVICE           0x00000002
#define FILE_FLOPPY_DISKETTE            0x00000004

#define DO_VERIFY_VOLUME                0x00000002
#define DO_DEVICE_INITIALIZING          0x00000080

/* ---- IOCTLs (NT values where they exist) -------------------------------- */

#define CTL_CODE(type, func, method, access) (((type) << 16) | ((access) << 14) | ((func) << 2) | (method))
#define IOCTL_DISK_GET_DRIVE_GEOMETRY       0x00070000
#define IOCTL_DISK_GET_PARTITION_INFO_EX    0x00070048
#define IOCTL_DISK_GET_LENGTH_INFO          0x0007405C
#define IOCTL_STORAGE_CHECK_VERIFY          0x002D4800
#define IOCTL_XENON_QUERY_DISK              CTL_CODE(0x8000, 0x800, 0, 0)

typedef struct {
    int64_t  Cylinders;
    uint32_t MediaType;             /* 12 = FixedMedia, 11 = RemovableMedia, 2 = F3_1Pt44_512 ... */
    uint32_t TracksPerCylinder;
    uint32_t SectorsPerTrack;
    uint32_t BytesPerSector;
} DISK_GEOMETRY;

typedef struct { int64_t Length; } GET_LENGTH_INFORMATION;

typedef struct {
    uint32_t PartitionStyle;        /* 0 MBR, 1 GPT, 2 RAW (whole disk) */
    int64_t  StartingOffset;
    int64_t  PartitionLength;
    uint32_t PartitionNumber;
    uint8_t  MbrType;
    bool     BootIndicator;
    uint8_t  GptType[16];
    char     Name[37];              /* GPT partition name (ASCII) */
    bool     Logical;               /* MBR logical partition (inside an extended one) */
    uint32_t TableIndex;            /* 1-based: MBR 1-4 primary, 5+ logical; GPT entry + 1 */
    uint32_t MbrDiskId;             /* disk signature (MBR) */
    uint8_t  GptPartUuid[16];
} PARTITION_INFORMATION_EX;

typedef struct {
    char     Model[41];
    char     Serial[21];
    char     Bus[40];
    uint32_t BytesPerSector;
    uint64_t SectorCount;
    bool     Removable;
    bool     MediaPresent;
    bool     WriteProtected;
    uint32_t DiskNumber;            /* N in HarddiskN / FloppyN */
    bool     Floppy;
    uint32_t MediaChangeCount;      /* goes up every time the medium changes */
} XENON_DISK_INFO;

/* ---- Objects ------------------------------------------------------------ */

typedef struct IO_STATUS_BLOCK {
    NTSTATUS Status;
    uint64_t Information;           /* usually bytes transferred */
} IO_STATUS_BLOCK;

struct DEVICE_OBJECT;
struct IRP;
struct FILE_OBJECT;
struct VPB;
typedef NTSTATUS (*PDRIVER_DISPATCH)(struct DEVICE_OBJECT *dev, struct IRP *irp);
typedef NTSTATUS (*PIO_COMPLETION_ROUTINE)(struct DEVICE_OBJECT *dev, struct IRP *irp, void *context);

typedef struct DRIVER_OBJECT {
    char DriverName[32];
    PDRIVER_DISPATCH MajorFunction[IRP_MJ_MAXIMUM_FUNCTION + 1];
    void (*DriverUnload)(struct DRIVER_OBJECT *driver);
    struct DEVICE_OBJECT *DeviceObject;      /* list of its devices (NextDevice) */
    void *DriverExtension;
} DRIVER_OBJECT;

typedef struct DEVICE_OBJECT {
    DRIVER_OBJECT *DriverObject;
    struct DEVICE_OBJECT *NextDevice;        /* next device of the same driver */
    struct DEVICE_OBJECT *AttachedDevice;    /* device stacked on top of this one */
    struct DEVICE_OBJECT *AttachedTo;        /* device this one is stacked on */
    uint32_t DeviceType;
    uint32_t Characteristics;
    uint32_t Flags;
    uint32_t SectorSize;
    int8_t   StackSize;                      /* IRP stack locations this device needs */
    void    *DeviceExtension;
    struct VPB *Vpb;                         /* volumes: the mounted file system */
} DEVICE_OBJECT;

typedef struct IO_STACK_LOCATION {
    uint8_t MajorFunction;
    uint8_t MinorFunction;
    uint8_t Flags;
    uint8_t Control;
    union {
        struct { uint32_t Length; uint64_t ByteOffset; } Read;
        struct { uint32_t Length; uint64_t ByteOffset; } Write;
        struct { uint32_t OutputBufferLength; uint32_t InputBufferLength; uint32_t IoControlCode; } DeviceIoControl;
        struct { uint32_t Options; uint32_t FileAttributes; uint32_t DesiredAccess; } Create;
        struct { uint32_t Length; uint32_t FileInformationClass; } QueryFile;
        struct { uint32_t Length; uint32_t FileInformationClass; struct FILE_OBJECT *TargetFileObject;
                 const char *TargetName; bool ReplaceIfExists; } SetFile;
        struct { uint32_t Length; uint32_t FileInformationClass; const char *FileName; uint32_t Flags; } QueryDirectory;
        struct { uint32_t Length; uint32_t FsInformationClass; } QueryVolume;
        struct { struct VPB *Vpb; struct DEVICE_OBJECT *DeviceObject; uint32_t FsControlCode; } FileSystemControl;
        struct { void *Arg1, *Arg2, *Arg3, *Arg4; } Others;
    } Parameters;
    DEVICE_OBJECT *DeviceObject;
    struct FILE_OBJECT *FileObject;
    PIO_COMPLETION_ROUTINE CompletionRoutine;
    void *Context;
} IO_STACK_LOCATION;

typedef struct IRP {
    IO_STATUS_BLOCK IoStatus;
    void *SystemBuffer;             /* data for read/write, in/out buffer for IOCTLs */
    KEVENT *UserEvent;              /* set on completion (synchronous requests) */
    IO_STATUS_BLOCK *UserIosb;      /* copied out on completion */
    int8_t StackCount;
    int8_t CurrentLocation;         /* index of the current stack location */
    bool PendingReturned;
    bool Cancel;
    uint32_t Flags;
    IO_STACK_LOCATION Stack[];
} IRP;

#define IRP_FREE_ON_COMPLETE 0x1

extern OBJECT_TYPE *IoDriverObjectType, *IoDeviceObjectType;

void IoInitSystem(void);

/* Built-in drivers: create \Driver\<name> and call its entry point. */
NTSTATUS IoCreateDriver(const char *name, NTSTATUS (*entry)(DRIVER_OBJECT *driver));
NTSTATUS IoCreateDevice(DRIVER_OBJECT *driver, size_t extension_size, const char *name, uint32_t type,
                        uint32_t characteristics, DEVICE_OBJECT **device);
void IoDeleteDevice(DEVICE_OBJECT *device);
DEVICE_OBJECT *IoAttachDeviceToDeviceStack(DEVICE_OBJECT *source, DEVICE_OBJECT *target);
NTSTATUS IoCreateSymbolicLink(const char *link, const char *target);
NTSTATUS IoDeleteSymbolicLink(const char *link);
/* Look up a device by name (\Device\..., \GLOBAL??\..., or a bare name like
   "PhysicalDrive0" which is looked up in \GLOBAL??). Returns a reference. */
NTSTATUS IoGetDeviceObjectPointer(const char *name, DEVICE_OBJECT **device);
DEVICE_OBJECT *IoGetAttachedDevice(DEVICE_OBJECT *device);   /* top of its stack */

IRP *IoAllocateIrp(int8_t stack_size);
void IoFreeIrp(IRP *irp);
NTSTATUS IoCallDriver(DEVICE_OBJECT *device, IRP *irp);
void IoCompleteRequest(IRP *irp);

static inline IO_STACK_LOCATION *IoGetCurrentIrpStackLocation(IRP *irp) { return &irp->Stack[irp->CurrentLocation]; }
static inline IO_STACK_LOCATION *IoGetNextIrpStackLocation(IRP *irp) { return &irp->Stack[irp->CurrentLocation - 1]; }
static inline void IoSkipCurrentIrpStackLocation(IRP *irp) { irp->CurrentLocation++; }
static inline void IoMarkIrpPending(IRP *irp) { irp->PendingReturned = true; }
void IoCopyCurrentIrpStackLocationToNext(IRP *irp);
void IoSetCompletionRoutine(IRP *irp, PIO_COMPLETION_ROUTINE routine, void *context);

/* Build and send a request, and wait for it. */
NTSTATUS IoSynchronousReadWrite(DEVICE_OBJECT *device, bool write, void *buffer, uint32_t length,
                                uint64_t offset, uint64_t *transferred);
NTSTATUS IoSynchronousDeviceControl(DEVICE_OBJECT *device, uint32_t code, void *in, uint32_t in_len,
                                    void *out, uint32_t out_len, uint64_t *returned);
NTSTATUS IoSynchronousFlush(DEVICE_OBJECT *device);

/* Called before restart/power-off: flush every disk cache and drive cache. */
void IoShutdownSystem(void);

/* ---- Disk class (used by storage port drivers) -------------------------- */

typedef struct DISK_PORT_OPS {
    NTSTATUS (*Read)(void *ctx, uint64_t lba, uint32_t count, void *buffer);
    NTSTATUS (*Write)(void *ctx, uint64_t lba, uint32_t count, const void *buffer);
    NTSTATUS (*Flush)(void *ctx);                        /* may be NULL */
    /* Removable media: refresh presence/size; sets *changed if the medium
       was swapped since the last call. May be NULL for fixed disks. */
    NTSTATUS (*CheckMedia)(void *ctx, bool *changed);
} DISK_PORT_OPS;

typedef struct DISK_PARAMS {
    bool     Floppy;                /* \Device\FloppyN instead of \Device\HarddiskN\DR0 */
    bool     Removable;
    const char *Model;
    const char *Serial;
    const char *Bus;                /* e.g. "IDE primary master" */
    uint32_t BytesPerSector;
    uint64_t SectorCount;           /* 0 if no medium yet */
    uint32_t MaxTransferSectors;    /* largest single request the port accepts */
    const DISK_PORT_OPS *Ops;
    void    *Context;
} DISK_PARAMS;

/* Create a disk device, register its names and scan its partitions. */
NTSTATUS DiskCreate(const DISK_PARAMS *params, DEVICE_OBJECT **disk);
/* Port drivers call this when media appears, disappears or changes size. */
void DiskSetMedia(DEVICE_OBJECT *disk, bool present, uint64_t sectors, uint32_t bytes_per_sector,
                  bool write_protected);
unsigned DiskCount(void);
DEVICE_OBJECT *DiskGet(unsigned index);                  /* all disks and floppies */

/* ---- Block cache ---------------------------------------------------------- */

typedef struct CC_STATS {
    uint64_t Lines, DirtyLines, CapacityLines;
    uint64_t Hits, Misses, ReadsFromDisk, WritesToDisk, BypassReads, BypassWrites;
} CC_STATS;

struct CACHE;
struct CACHE *CcCreate(DEVICE_OBJECT *disk, bool write_through);
NTSTATUS CcRead(struct CACHE *c, uint64_t offset, uint32_t length, void *buffer);
NTSTATUS CcWrite(struct CACHE *c, uint64_t offset, uint32_t length, const void *buffer);
NTSTATUS CcFlush(struct CACHE *c);
void CcInvalidate(struct CACHE *c);                      /* drop everything (media change) */
void CcQueryStats(CC_STATS *stats);
void CcStartLazyWriter(void);
NTSTATUS CcFlushAll(void);

/* ---- Partition manager ----------------------------------------------------- */

void PartMgrScanDisk(DEVICE_OBJECT *disk);
void PartMgrRemoveDisk(DEVICE_OBJECT *disk);
unsigned PartMgrVolumeCount(void);
DEVICE_OBJECT *PartMgrGetVolume(unsigned index);
/* Partition table type of a disk: "MBR", "GPT", "none" (superfloppy) or "?" */
const char *PartMgrDiskStyle(DEVICE_OBJECT *disk);

uint32_t RtlCrc32(uint32_t crc, const void *data, size_t len);

/* ---- Files, volumes and file systems ---------------------------------------- */

#define IRP_MN_MOUNT_VOLUME        0x01
#define IRP_MN_USER_FS_REQUEST     0x00
#define IRP_MN_QUERY_DIRECTORY     0x01
#define FSCTL_DISMOUNT_VOLUME      0x00090020
#define FSCTL_XENON_FORCE_DISMOUNT 0x80000004   /* write everything and dismount even with open files */
#define FSCTL_XENON_INVALIDATE     0x80000008   /* medium changed or gone: dismount without writing */

/* NtCreateFile dispositions and options (NT values) */
#define FILE_SUPERSEDE             0
#define FILE_OPEN                  1
#define FILE_CREATE                2
#define FILE_OPEN_IF               3
#define FILE_OVERWRITE             4
#define FILE_OVERWRITE_IF          5
#define FILE_DIRECTORY_FILE        0x00000001
#define FILE_NON_DIRECTORY_FILE    0x00000040
#define FILE_DELETE_ON_CLOSE       0x00001000
/* results (IO_STATUS_BLOCK.Information) */
#define FILE_SUPERSEDED            0
#define FILE_OPENED                1
#define FILE_CREATED               2
#define FILE_OVERWRITTEN           3

/* Access rights (subset) */
#define FILE_READ_DATA             0x0001
#define FILE_WRITE_DATA            0x0002
#define FILE_APPEND_DATA           0x0004
#define DELETE                     0x00010000
#define GENERIC_READ               0x80000000u
#define GENERIC_WRITE              0x40000000u
#define GENERIC_ALL                0x10000000u

/* File attributes (same bits as FAT) */
#define FILE_ATTRIBUTE_READONLY    0x01
#define FILE_ATTRIBUTE_HIDDEN      0x02
#define FILE_ATTRIBUTE_SYSTEM      0x04
#define FILE_ATTRIBUTE_DIRECTORY   0x10
#define FILE_ATTRIBUTE_ARCHIVE     0x20
#define FILE_ATTRIBUTE_NORMAL      0x80

/* Information classes (NT numbers) */
#define FileDirectoryInformation    1
#define FileBasicInformation        4
#define FileStandardInformation     5
#define FileNameInformation         9
#define FileRenameInformation       10
#define FileDispositionInformation  13
#define FilePositionInformation     14
#define FileEndOfFileInformation    20
#define FileFsVolumeInformation     1
#define FileFsSizeInformation       3
#define FileFsAttributeInformation  5

/* Times are NT times: 100 ns units since 1601-01-01 UTC. */
typedef struct { int64_t CreationTime, LastAccessTime, LastWriteTime, ChangeTime; uint32_t FileAttributes; } FILE_BASIC_INFORMATION;
typedef struct { int64_t AllocationSize, EndOfFile; uint32_t NumberOfLinks; bool DeletePending, Directory; } FILE_STANDARD_INFORMATION;
typedef struct { int64_t CurrentByteOffset; } FILE_POSITION_INFORMATION;
typedef struct { int64_t EndOfFile; } FILE_END_OF_FILE_INFORMATION;
typedef struct { bool DeleteFile; } FILE_DISPOSITION_INFORMATION;
typedef struct { uint32_t FileNameLength; char FileName[1]; } FILE_NAME_INFORMATION;       /* UTF-8 */

/* Directory entries (UTF-8 names; NextEntryOffset 0 ends the buffer). */
typedef struct {
    uint32_t NextEntryOffset;
    uint32_t FileIndex;
    int64_t  CreationTime, LastAccessTime, LastWriteTime, ChangeTime;
    int64_t  EndOfFile, AllocationSize;
    uint32_t FileAttributes;
    char     ShortName[13];
    uint32_t FileNameLength;
    char     FileName[1];
} FILE_DIRECTORY_INFORMATION;
#define SL_RESTART_SCAN        0x01
#define SL_RETURN_SINGLE_ENTRY 0x02

typedef struct { int64_t VolumeCreationTime; uint32_t VolumeSerialNumber; uint32_t VolumeLabelLength; char VolumeLabel[32]; } FILE_FS_VOLUME_INFORMATION;
typedef struct { int64_t TotalAllocationUnits, AvailableAllocationUnits; uint32_t SectorsPerAllocationUnit, BytesPerSector; } FILE_FS_SIZE_INFORMATION;
typedef struct { uint32_t FileSystemAttributes; uint32_t MaximumComponentNameLength; uint32_t FileSystemNameLength; char FileSystemName[16]; } FILE_FS_ATTRIBUTE_INFORMATION;

/* Volume parameter block: links a volume device to its mounted file system. */
#define VPB_MOUNTED 0x0001
typedef struct VPB {
    uint32_t Flags;
    DEVICE_OBJECT *DeviceObject;        /* the file system's volume device */
    DEVICE_OBJECT *RealDevice;          /* the storage volume (partition, floppy) */
    uint32_t SerialNumber;
    char VolumeLabel[32];
    char FileSystem[16];                /* "FAT12", "FAT16", "FAT32" */
    uint32_t MediaChangeCount;          /* of the real device when mounted */
    volatile uint32_t ReferenceCount;   /* open files */
} VPB;

#define FO_VOLUME_OPEN   0x0001         /* the volume itself, not a file in it */
#define FO_RAW_DEVICE    0x0002         /* a device with no file system (e.g. a whole disk) */
#define FO_DIRECTORY     0x0004

typedef struct FILE_OBJECT {
    DEVICE_OBJECT *DeviceObject;        /* the real device (volume, disk) */
    VPB *Vpb;                           /* mounted file system, NULL for raw devices */
    DEVICE_OBJECT *FsDevice;            /* the file system volume device it was opened on */
    void *FsContext;                    /* file system: shared per-file data */
    void *FsContext2;                   /* file system: per-open data */
    int64_t CurrentByteOffset;
    uint32_t Flags;
    uint32_t DesiredAccess;
    bool DeletePending;
    char FileName[512];                 /* path inside the volume, '\'-separated */
} FILE_OBJECT;

extern OBJECT_TYPE *IoFileObjectType;

NTSTATUS IoRegisterFileSystem(DEVICE_OBJECT *fs_device);
/* Mount a file system on a volume device if none is mounted yet (or the
   medium changed). Returns STATUS_UNRECOGNIZED_VOLUME if no file system knows it. */
NTSTATUS IoMountVolume(DEVICE_OBJECT *volume);
NTSTATUS IoDismountVolume(DEVICE_OBJECT *volume);
NTSTATUS IoForceDismountVolume(DEVICE_OBJECT *volume);   /* shutdown, removed media */
bool IoIsVolumeDevice(DEVICE_OBJECT *device);
void IoMarkVolumeDevice(DEVICE_OBJECT *device);   /* partitions and floppies */
NTSTATUS IoQueryMediaChangeCount(DEVICE_OBJECT *device, uint32_t *count);

/* ---- Native file API (kernel callers now, system calls in Phase 1.5) ------- */

/* "C:/dir/file" or "C:\dir\file" -> "\??\C:\dir\file"; "//./X" -> "\??\X";
   NT paths ("\Device\...") are copied. */
NTSTATUS RtlDosPathToNtPath(const char *dos, char *nt, size_t size);

NTSTATUS NtCreateFile(HANDLE *handle, uint32_t access, const char *path, IO_STATUS_BLOCK *iosb,
                      uint32_t attributes, uint32_t disposition, uint32_t options);
NTSTATUS NtReadFile(HANDLE h, void *buf, uint32_t len, const int64_t *offset, IO_STATUS_BLOCK *iosb);
NTSTATUS NtWriteFile(HANDLE h, const void *buf, uint32_t len, const int64_t *offset, IO_STATUS_BLOCK *iosb);
NTSTATUS NtQueryInformationFile(HANDLE h, IO_STATUS_BLOCK *iosb, void *buf, uint32_t len, uint32_t cls);
NTSTATUS NtSetInformationFile(HANDLE h, IO_STATUS_BLOCK *iosb, void *buf, uint32_t len, uint32_t cls);
/* cls == FileRenameInformation: buf is the new path (DOS or NT form, NUL-terminated). */
NTSTATUS NtQueryDirectoryFile(HANDLE h, IO_STATUS_BLOCK *iosb, void *buf, uint32_t len, uint32_t cls,
                              bool single, const char *pattern, bool restart);
NTSTATUS NtQueryVolumeInformationFile(HANDLE h, IO_STATUS_BLOCK *iosb, void *buf, uint32_t len, uint32_t cls);
NTSTATUS NtFlushBuffersFile(HANDLE h, IO_STATUS_BLOCK *iosb);
NTSTATUS NtDeleteFile(const char *path);
NTSTATUS NtClose(HANDLE h);

/* Time helpers */
int64_t KeQuerySystemTime(void);                    /* NT time, now */
int64_t RtlUnixToNtTime(int64_t unix_seconds);
int64_t RtlNtTimeToUnix(int64_t nt);

/* ---- Mount manager (drive letters) ------------------------------------------- */

typedef struct {
    char Letter;                    /* 'A'..'Z' */
    char Device[64];                /* \Device\HarddiskVolume1, \Device\Floppy0 */
    char FileSystem[16];            /* "" if no file system (e.g. empty floppy drive) */
    char Label[32];
    uint32_t Serial;
    uint64_t TotalBytes, FreeBytes;
    bool Removable, Floppy, Boot, MediaPresent;
} MOUNT_INFO;

void MountMgrInitialize(void);                      /* assign letters to what exists at boot */
void MountMgrVolumeArrival(DEVICE_OBJECT *volume);  /* later arrivals (USB, ...) */
void MountMgrVolumeRemoval(DEVICE_OBJECT *volume);
NTSTATUS MountMgrAssign(char letter, const char *device);
NTSTATUS MountMgrRemove(char letter);
bool MountMgrQuery(char letter, MOUNT_INFO *info);  /* false if the letter is unused */
void MountMgrShutdown(void);                        /* dismount everything cleanly */
