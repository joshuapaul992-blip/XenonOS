/* SPDX-License-Identifier: MIT */
/*
 * Mount manager: drive letters.
 *
 * Rules (from PLAN.md):
 *   A:, B:  floppy drives 0 and 1, whether or not a disk is in them
 *   C:      the volume XenonOS was booted from (identified from what the
 *           bootloader reports: MBR disk signature + partition number, or the
 *           GPT partition GUID; failing that, the first volume holding
 *           \XenonOS\System\xnkrnl.elf)
 *   D: ...  other FAT volumes on fixed disks: primary partitions of every
 *           disk first, then logical partitions
 *   then    removable volumes (USB sticks, ...) in arrival order
 * Letters are sticky: C:\XenonOS\Config\mountmgr.cfg remembers which volume
 * (serial number, partition offset and size) had which letter, and gives it
 * the same letter next time. Volumes without a FAT file system get no letter.
 *
 * A letter is a symbolic link \GLOBAL??\X: -> the volume device, as on NT.
 */
#include "io.h"
#include "mm.h"
#include "rtl.h"
#include "loader.h"
#include "console.h"

#define CONFIG_PATH "C:\\XenonOS\\Config\\mountmgr.cfg"
#define MAX_STICKY 32

typedef struct {
    DEVICE_OBJECT *Dev;
    bool Floppy, Boot;
} LETTER;

typedef struct {
    char Letter;
    uint32_t Serial;
    uint64_t Offset, Length;
} STICKY;

static LETTER g_letters[26];
static STICKY g_sticky[MAX_STICKY];
static unsigned g_sticky_count;
static bool g_ready;
static KMUTEX g_lock;

/* ---- Volume facts ------------------------------------------------------------ */

typedef struct {
    DEVICE_OBJECT *Dev;
    char Name[64];
    bool Removable, Logical, Gpt;
    uint32_t Disk, Partition, TableIndex, MbrDiskId;
    uint8_t GptPartUuid[16];
    uint64_t Offset, Length;
    uint32_t Serial;
} VOLUME;

static bool describe(DEVICE_OBJECT *dev, VOLUME *v)
{
    memset(v, 0, sizeof(*v));
    v->Dev = dev;
    ObQueryNameString(dev, v->Name, sizeof(v->Name));
    XENON_DISK_INFO di;
    PARTITION_INFORMATION_EX pi;
    if (!NT_SUCCESS(IoSynchronousDeviceControl(dev, IOCTL_XENON_QUERY_DISK, NULL, 0, &di, sizeof(di), NULL)))
        return false;
    v->Removable = di.Removable;
    v->Disk = di.DiskNumber;
    if (NT_SUCCESS(IoSynchronousDeviceControl(dev, IOCTL_DISK_GET_PARTITION_INFO_EX, NULL, 0, &pi, sizeof(pi), NULL))) {
        v->Logical = pi.Logical;
        v->Gpt = pi.PartitionStyle == 1;
        v->Partition = pi.PartitionNumber;
        v->TableIndex = pi.TableIndex;
        v->MbrDiskId = pi.MbrDiskId;
        memcpy(v->GptPartUuid, pi.GptPartUuid, 16);
        v->Offset = (uint64_t)pi.StartingOffset;
        v->Length = (uint64_t)pi.PartitionLength;
    }
    if (dev->Vpb && (dev->Vpb->Flags & VPB_MOUNTED))
        v->Serial = dev->Vpb->SerialNumber;
    return true;
}

static bool is_boot_volume(const VOLUME *v)
{
    LOADER_BLOCK *lb = &KeLoaderBlock;
    if (!lb->HaveBootPartition || !lb->BootPartitionIndex)
        return false;
    if (lb->BootGpt)
        return v->Gpt && !memcmp(v->GptPartUuid, lb->BootGptPartUuid, 16);
    return !v->Gpt && v->MbrDiskId == lb->BootMbrDiskId && v->TableIndex == lb->BootPartitionIndex;
}

static bool holds_kernel(const VOLUME *v)
{
    char path[128];
    snprintf(path, sizeof(path), "%s\\XenonOS\\System\\xnkrnl.elf", v->Name);
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    if (!NT_SUCCESS(NtCreateFile(&h, FILE_READ_DATA, path, &iosb, 0, FILE_OPEN, FILE_NON_DIRECTORY_FILE)))
        return false;
    NtClose(h);
    return true;
}

/* ---- Letters ------------------------------------------------------------------- */

static NTSTATUS link_letter(char letter, DEVICE_OBJECT *dev, bool floppy, bool boot)
{
    char link[16], target[64];
    snprintf(link, sizeof(link), "\\GLOBAL??\\%c:", letter);
    ObQueryNameString(dev, target, sizeof(target));
    NTSTATUS st = IoCreateSymbolicLink(link, target);
    if (NT_SUCCESS(st)) {
        g_letters[letter - 'A'] = (LETTER){ dev, floppy, boot };
        ObReferenceObject(dev);
    }
    return st;
}

static void unlink_letter(char letter)
{
    char link[16];
    snprintf(link, sizeof(link), "\\GLOBAL??\\%c:", letter);
    IoDeleteSymbolicLink(link);
    if (g_letters[letter - 'A'].Dev)
        ObDereferenceObject(g_letters[letter - 'A'].Dev);
    g_letters[letter - 'A'] = (LETTER){ 0 };
}

static char letter_of(DEVICE_OBJECT *dev)
{
    for (int i = 0; i < 26; i++)
        if (g_letters[i].Dev == dev)
            return (char)('A' + i);
    return 0;
}

static char free_letter(void)
{
    for (int i = 'D' - 'A'; i < 26; i++)
        if (!g_letters[i].Dev)
            return (char)('A' + i);
    return 0;
}

static char sticky_letter(const VOLUME *v)
{
    for (unsigned i = 0; i < g_sticky_count; i++) {
        STICKY *s = &g_sticky[i];
        if (s->Serial == v->Serial && s->Offset == v->Offset && s->Length == v->Length &&
            s->Letter >= 'D' && !g_letters[s->Letter - 'A'].Dev)
            return s->Letter;
    }
    return 0;
}

static void log_letter(char letter, const char *why)
{
    LETTER *l = &g_letters[letter - 'A'];
    char name[64];
    ObQueryNameString(l->Dev, name, sizeof(name));
    VPB *vpb = l->Dev->Vpb;
    bool mounted = vpb && (vpb->Flags & VPB_MOUNTED);
    DbgPrint("MountMgr: %c: -> %s%s%s%s%s%s", letter, name, mounted ? " (" : "", mounted ? vpb->FileSystem : "",
             mounted && vpb->VolumeLabel[0] ? " \"" : "", mounted ? vpb->VolumeLabel : "",
             mounted ? (vpb->VolumeLabel[0] ? "\")" : ")") : (l->Floppy ? " (no disk)" : ""));
    UNUSED(why);
}

/* ---- Sticky letters ------------------------------------------------------------ */

static void load_config(void)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    if (!NT_SUCCESS(NtCreateFile(&h, FILE_READ_DATA, CONFIG_PATH, &iosb, 0, FILE_OPEN, FILE_NON_DIRECTORY_FILE)))
        return;
    char *buf = ExAllocatePoolWithTag(NonPagedPoolZeroed, 4096, 'MntM');
    if (buf && NT_SUCCESS(NtReadFile(h, buf, 4095, NULL, &iosb))) {
        buf[iosb.Information] = 0;
        char *line = buf;
        while (*line && g_sticky_count < MAX_STICKY) {
            char *end = strchr(line, '\n');
            if (end)
                *end = 0;
            /* "E: serial=1234ABCD offset=1048576 length=10485760" */
            if (line[0] >= 'A' && line[0] <= 'Z' && line[1] == ':') {
                STICKY s = { .Letter = line[0] };
                const char *p;
                if ((p = strstr(line, "serial="))) {
                    for (p += 7; *p && *p != ' '; p++)
                        s.Serial = s.Serial * 16 + (uint32_t)(*p <= '9' ? *p - '0' : (*p | 0x20) - 'a' + 10);
                }
                if ((p = strstr(line, "offset=")))
                    for (p += 7; *p >= '0' && *p <= '9'; p++)
                        s.Offset = s.Offset * 10 + (uint64_t)(*p - '0');
                if ((p = strstr(line, "length=")))
                    for (p += 7; *p >= '0' && *p <= '9'; p++)
                        s.Length = s.Length * 10 + (uint64_t)(*p - '0');
                g_sticky[g_sticky_count++] = s;
            }
            if (!end)
                break;
            line = end + 1;
        }
    }
    if (buf)
        ExFreePoolWithTag(buf, 'MntM');
    NtClose(h);
}

static void save_config(void)
{
    if (!g_letters['C' - 'A'].Dev)
        return;
    /* Remember the letters in use now, plus remembered ones whose volume is absent. */
    STICKY now[MAX_STICKY];
    unsigned n = 0;
    for (int i = 'D' - 'A'; i < 26 && n < MAX_STICKY; i++) {
        VOLUME v;
        if (g_letters[i].Dev && !g_letters[i].Floppy && describe(g_letters[i].Dev, &v) && v.Serial)
            now[n++] = (STICKY){ (char)('A' + i), v.Serial, v.Offset, v.Length };
    }
    for (unsigned i = 0; i < g_sticky_count && n < MAX_STICKY; i++) {
        bool taken = false;
        for (unsigned k = 0; k < n; k++)
            taken |= now[k].Letter == g_sticky[i].Letter ||
                     (now[k].Serial == g_sticky[i].Serial && now[k].Offset == g_sticky[i].Offset);
        if (!taken)
            now[n++] = g_sticky[i];
    }
    bool same = n == g_sticky_count;
    for (unsigned i = 0; same && i < n; i++)
        same = !memcmp(&now[i], &g_sticky[i], sizeof(STICKY));
    if (same)
        return;
    memcpy(g_sticky, now, sizeof(STICKY) * n);
    g_sticky_count = n;

    char *buf = ExAllocatePoolWithTag(NonPagedPool, 4096, 'MntM');
    if (!buf)
        return;
    int len = snprintf(buf, 4096, "# XenonOS drive letters - written by the mount manager\n");
    for (unsigned i = 0; i < n; i++)
        len += snprintf(buf + len, 4096 - len, "%c: serial=%08X offset=%llu length=%llu\n", now[i].Letter,
                        now[i].Serial, (unsigned long long)now[i].Offset, (unsigned long long)now[i].Length);
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    if (NT_SUCCESS(NtCreateFile(&h, FILE_WRITE_DATA, CONFIG_PATH, &iosb, 0, FILE_OVERWRITE_IF, FILE_NON_DIRECTORY_FILE))) {
        NtWriteFile(h, buf, (uint32_t)len, NULL, &iosb);
        NtClose(h);
    }
    ExFreePoolWithTag(buf, 'MntM');
}

/* ---- Boot-time assignment -------------------------------------------------------- */

void MountMgrInitialize(void)
{
    KeInitializeMutex(&g_lock);
    KeWaitForSingleObject(&g_lock, KE_INFINITE);

    /* A: and B: */
    for (unsigned i = 0; i < DiskCount(); i++) {
        DEVICE_OBJECT *d = DiskGet(i);
        XENON_DISK_INFO di;
        if (!NT_SUCCESS(IoSynchronousDeviceControl(d, IOCTL_XENON_QUERY_DISK, NULL, 0, &di, sizeof(di), NULL)) ||
            !di.Floppy || di.DiskNumber > 1)
            continue;
        char letter = (char)('A' + di.DiskNumber);
        if (NT_SUCCESS(link_letter(letter, d, true, false)))
            log_letter(letter, "floppy");
    }

    /* Every FAT volume. */
    unsigned count = PartMgrVolumeCount();
    VOLUME *vols = count ? ExAllocatePoolWithTag(NonPagedPoolZeroed, sizeof(VOLUME) * count, 'MntM') : NULL;
    unsigned nv = 0;
    for (unsigned i = 0; vols && i < count; i++) {
        DEVICE_OBJECT *d = PartMgrGetVolume(i);
        if (!d || !NT_SUCCESS(IoMountVolume(d)))
            continue;                               /* no FAT file system: no letter */
        if (describe(d, &vols[nv]))
            nv++;
    }

    /* C: */
    int boot = -1;
    for (unsigned i = 0; i < nv && boot < 0; i++)
        if (is_boot_volume(&vols[i]))
            boot = (int)i;
    if (boot < 0)
        for (unsigned i = 0; i < nv && boot < 0; i++)
            if (holds_kernel(&vols[i]))
                boot = (int)i;
    if (boot >= 0) {
        if (NT_SUCCESS(link_letter('C', vols[boot].Dev, false, true)))
            log_letter('C', "boot");
        load_config();
    } else {
        DbgPrint("MountMgr: the boot volume was not found - no drive C:");
    }

    /* Remembered letters first, then fixed primaries, fixed logicals, removable. */
    for (int pass = 0; pass < 4; pass++) {
        for (unsigned i = 0; i < nv; i++) {
            VOLUME *v = &vols[i];
            if ((int)i == boot || letter_of(v->Dev))
                continue;
            char letter = 0;
            if (pass == 0) letter = sticky_letter(v);
            else if (pass == 1 && !v->Removable && !v->Logical) letter = free_letter();
            else if (pass == 2 && !v->Removable && v->Logical) letter = free_letter();
            else if (pass == 3 && v->Removable) letter = free_letter();
            if (!letter)
                continue;
            if (NT_SUCCESS(link_letter(letter, v->Dev, false, false)))
                log_letter(letter, "volume");
        }
    }
    if (vols)
        ExFreePoolWithTag(vols, 'MntM');
    save_config();
    g_ready = true;
    KeReleaseMutex(&g_lock);
}

void MountMgrVolumeArrival(DEVICE_OBJECT *dev)
{
    if (!g_ready || !NT_SUCCESS(IoMountVolume(dev)))
        return;
    KeWaitForSingleObject(&g_lock, KE_INFINITE);
    VOLUME v;
    if (!letter_of(dev) && describe(dev, &v)) {
        char letter = sticky_letter(&v);
        if (!letter)
            letter = free_letter();
        if (letter && NT_SUCCESS(link_letter(letter, dev, false, false))) {
            log_letter(letter, "arrival");
            save_config();
        }
    }
    KeReleaseMutex(&g_lock);
}

void MountMgrVolumeRemoval(DEVICE_OBJECT *dev)
{
    if (!g_ready)
        return;
    KeWaitForSingleObject(&g_lock, KE_INFINITE);
    char letter = letter_of(dev);
    if (letter && !g_letters[letter - 'A'].Floppy) {
        DbgPrint("MountMgr: %c: removed", letter);
        unlink_letter(letter);
    }
    KeReleaseMutex(&g_lock);
}

NTSTATUS MountMgrAssign(char letter, const char *device)
{
    letter = (char)toupper(letter);
    if (letter < 'A' || letter > 'Z')
        return STATUS_INVALID_PARAMETER;
    DEVICE_OBJECT *dev;
    NTSTATUS st = IoGetDeviceObjectPointer(device, &dev);
    if (!NT_SUCCESS(st))
        return st;
    KeWaitForSingleObject(&g_lock, KE_INFINITE);
    if (g_letters[letter - 'A'].Dev)
        st = STATUS_OBJECT_NAME_COLLISION;
    else if (letter_of(dev))
        st = STATUS_OBJECT_NAME_EXISTS;
    else if (!IoIsVolumeDevice(dev))
        st = STATUS_INVALID_DEVICE_REQUEST;
    else {
        st = IoMountVolume(dev);
        if (NT_SUCCESS(st) || (dev->Characteristics & FILE_FLOPPY_DISKETTE)) {
            st = link_letter(letter, dev, (dev->Characteristics & FILE_FLOPPY_DISKETTE) != 0, false);
            if (NT_SUCCESS(st)) {
                log_letter(letter, "assigned");
                save_config();
            }
        }
    }
    KeReleaseMutex(&g_lock);
    ObDereferenceObject(dev);
    return st;
}

NTSTATUS MountMgrRemove(char letter)
{
    letter = (char)toupper(letter);
    if (letter < 'A' || letter > 'Z')
        return STATUS_INVALID_PARAMETER;
    KeWaitForSingleObject(&g_lock, KE_INFINITE);
    LETTER *l = &g_letters[letter - 'A'];
    NTSTATUS st = STATUS_OBJECT_NAME_NOT_FOUND;
    if (l->Dev) {
        st = IoDismountVolume(l->Dev);              /* fails while files are open */
        if (NT_SUCCESS(st)) {
            DbgPrint("MountMgr: %c: dismounted and removed", letter);
            unlink_letter(letter);
        }
    }
    KeReleaseMutex(&g_lock);
    return st;
}

bool MountMgrQuery(char letter, MOUNT_INFO *info)
{
    letter = (char)toupper(letter);
    if (letter < 'A' || letter > 'Z' || !g_letters[letter - 'A'].Dev)
        return false;
    LETTER *l = &g_letters[letter - 'A'];
    memset(info, 0, sizeof(*info));
    info->Letter = letter;
    ObQueryNameString(l->Dev, info->Device, sizeof(info->Device));
    info->Floppy = l->Floppy;
    info->Boot = l->Boot;
    info->Removable = (l->Dev->Characteristics & FILE_REMOVABLE_MEDIA) != 0;
    /* Open the volume itself to ask its file system (this also mounts a
       newly inserted floppy). */
    char path[8];
    snprintf(path, sizeof(path), "%c:", letter);
    char nt[16];
    snprintf(nt, sizeof(nt), "\\??\\%c:", letter);
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    if (NT_SUCCESS(NtCreateFile(&h, FILE_READ_DATA, nt, &iosb, 0, FILE_OPEN, 0))) {
        info->MediaPresent = true;
        FILE_FS_SIZE_INFORMATION sz;
        FILE_FS_VOLUME_INFORMATION vol;
        FILE_FS_ATTRIBUTE_INFORMATION attr;
        if (NT_SUCCESS(NtQueryVolumeInformationFile(h, &iosb, &sz, sizeof(sz), FileFsSizeInformation))) {
            uint64_t unit = (uint64_t)sz.SectorsPerAllocationUnit * sz.BytesPerSector;
            info->TotalBytes = (uint64_t)sz.TotalAllocationUnits * unit;
            info->FreeBytes = (uint64_t)sz.AvailableAllocationUnits * unit;
        }
        if (NT_SUCCESS(NtQueryVolumeInformationFile(h, &iosb, &vol, sizeof(vol), FileFsVolumeInformation))) {
            info->Serial = vol.VolumeSerialNumber;
            strlcpy(info->Label, vol.VolumeLabel, sizeof(info->Label));
        }
        if (NT_SUCCESS(NtQueryVolumeInformationFile(h, &iosb, &attr, sizeof(attr), FileFsAttributeInformation)))
            strlcpy(info->FileSystem, attr.FileSystemName, sizeof(info->FileSystem));
        NtClose(h);
    }
    UNUSED(path);
    return true;
}

void MountMgrShutdown(void)
{
    if (!g_ready)
        return;
    for (int i = 0; i < 26; i++) {
        DEVICE_OBJECT *d = g_letters[i].Dev;
        if (d && d->Vpb && (d->Vpb->Flags & VPB_MOUNTED))
            IoForceDismountVolume(d);
    }
}
