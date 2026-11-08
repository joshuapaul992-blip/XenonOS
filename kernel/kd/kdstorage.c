/* SPDX-License-Identifier: MIT */
/*
 * Kd - storage commands for the kernel debug shell.
 *
 * Device names may be given as \Device\..., as \GLOBAL??\..., or as bare
 * DOS device names such as PhysicalDrive0, HarddiskVolume1 or Floppy0.
 */
#include "io.h"
#include "mm.h"
#include "rtl.h"
#include "console.h"

#define CHUNK (256 * 1024)

static DEVICE_OBJECT *open_dev(const char *name)
{
    DEVICE_OBJECT *d;
    NTSTATUS st = IoGetDeviceObjectPointer(name, &d);
    if (!NT_SUCCESS(st)) {
        ConPrint("%s: %s\n", name, RtlStatusName(st));
        return NULL;
    }
    return d;
}

static uint64_t parse_u64(const char *s)
{
    uint64_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        for (s += 2; *s; s++) {
            int d = (*s >= '0' && *s <= '9') ? *s - '0' : (tolower(*s) >= 'a' && tolower(*s) <= 'f') ? tolower(*s) - 'a' + 10 : -1;
            if (d < 0) break;
            v = v * 16 + (uint64_t)d;
        }
        return v;
    }
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (uint64_t)(*s++ - '0');
    return v;
}

static void size_string(uint64_t b, char *out, size_t n)
{
    if (b >= (10ull << 30)) snprintf(out, n, "%llu GiB", (unsigned long long)(b >> 30));
    else if (b >= (10ull << 20)) snprintf(out, n, "%llu MiB", (unsigned long long)(b >> 20));
    else snprintf(out, n, "%llu KiB", (unsigned long long)(b >> 10));
}

void KdCmdDisks(int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    ConPrint("Disk             Size       Model                     Table  Bus\n");
    for (unsigned i = 0; i < DiskCount(); i++) {
        DEVICE_OBJECT *d = DiskGet(i);
        XENON_DISK_INFO info;
        if (!NT_SUCCESS(IoSynchronousDeviceControl(d, IOCTL_XENON_QUERY_DISK, NULL, 0, &info, sizeof(info), NULL)))
            continue;
        char name[24], size[24];
        snprintf(name, sizeof(name), "%s%u", info.Floppy ? "Floppy" : "PhysicalDrive", info.DiskNumber);
        if (info.MediaPresent)
            size_string(info.SectorCount * info.BytesPerSector, size, sizeof(size));
        else
            strlcpy(size, "no medium", sizeof(size));
        ConPrint("%-16s %-10s %-25.25s %-6s %s\n", name, size, info.Model, PartMgrDiskStyle(d), info.Bus);
    }
}

void KdCmdVolumes(int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    ConPrint("Volume              Partition                      Offset       Size       Type\n");
    for (unsigned i = 0; i < PartMgrVolumeCount(); i++) {
        DEVICE_OBJECT *v = PartMgrGetVolume(i);
        PARTITION_INFORMATION_EX p;
        XENON_DISK_INFO info;
        if (!NT_SUCCESS(IoSynchronousDeviceControl(v, IOCTL_DISK_GET_PARTITION_INFO_EX, NULL, 0, &p, sizeof(p), NULL)) ||
            !NT_SUCCESS(IoSynchronousDeviceControl(v, IOCTL_XENON_QUERY_DISK, NULL, 0, &info, sizeof(info), NULL)))
            continue;
        char path[64], part[48], size[24], type[24];
        ObQueryNameString(v, path, sizeof(path));
        snprintf(part, sizeof(part), "Harddisk%u\\Partition%u", info.DiskNumber, p.PartitionNumber);
        size_string((uint64_t)p.PartitionLength, size, sizeof(size));
        if (p.PartitionStyle == 0) snprintf(type, sizeof(type), "MBR 0x%02x%s", p.MbrType, p.BootIndicator ? " *" : "");
        else if (p.PartitionStyle == 1) snprintf(type, sizeof(type), "GPT %s", p.Name);
        else strlcpy(type, "whole disk", sizeof(type));
        ConPrint("%-19s %-30s %-12llu %-10s %s\n", path + 8, part, (unsigned long long)p.StartingOffset, size, type);
    }
}

void KdCmdHexdump(int argc, char **argv)
{
    if (argc < 2) {
        ConPrint("usage: hexdump <device> [lba]\n");
        return;
    }
    DEVICE_OBJECT *d = open_dev(argv[1]);
    if (!d)
        return;
    uint64_t lba = argc > 2 ? parse_u64(argv[2]) : 0;
    uint32_t bps = d->SectorSize ? d->SectorSize : 512;
    uint8_t *buf = ExAllocatePoolWithTag(NonPagedPool, bps, 'KdSt');
    NTSTATUS st = buf ? IoSynchronousReadWrite(d, false, buf, bps, lba * bps, NULL) : STATUS_NO_MEMORY;
    if (!NT_SUCCESS(st)) {
        ConPrint("hexdump: %s\n", RtlStatusName(st));
    } else {
        for (uint32_t off = 0; off < bps; off += 16) {
            char line[80];
            int n = snprintf(line, sizeof(line), "%08llx ", (unsigned long long)(lba * bps + off));
            for (int i = 0; i < 16; i++)
                n += snprintf(line + n, sizeof(line) - n, "%s%02x", i == 8 ? "  " : " ", buf[off + i]);
            n += snprintf(line + n, sizeof(line) - n, "  ");
            for (int i = 0; i < 16; i++) {
                uint8_t c = buf[off + i];
                line[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
            }
            line[n] = 0;
            ConPrint("%s\n", line);
        }
    }
    if (buf)
        ExFreePoolWithTag(buf, 'KdSt');
    ObDereferenceObject(d);
}

static NTSTATUS device_length(DEVICE_OBJECT *d, uint64_t *len)
{
    GET_LENGTH_INFORMATION l;
    NTSTATUS st = IoSynchronousDeviceControl(d, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &l, sizeof(l), NULL);
    if (NT_SUCCESS(st))
        *len = (uint64_t)l.Length;
    return st;
}

void KdCmdCrc32(int argc, char **argv)
{
    if (argc < 2) {
        ConPrint("usage: crc32 <device> [first-lba [sector-count]]\n");
        return;
    }
    DEVICE_OBJECT *d = open_dev(argv[1]);
    if (!d)
        return;
    uint32_t bps = d->SectorSize ? d->SectorSize : 512;
    uint64_t total;
    NTSTATUS st = device_length(d, &total);
    uint64_t start = argc > 2 ? parse_u64(argv[2]) * bps : 0;
    if (NT_SUCCESS(st) && argc > 3)
        total = MIN(total, start + parse_u64(argv[3]) * bps);
    uint8_t *buf = ExAllocatePoolWithTag(NonPagedPool, CHUNK, 'KdSt');
    uint32_t crc = 0;
    uint64_t done = 0, t0 = KeQueryUptimeNs();
    for (uint64_t off = start; NT_SUCCESS(st) && buf && off < total; off += CHUNK) {
        uint32_t n = (uint32_t)MIN((uint64_t)CHUNK, total - off);
        st = IoSynchronousReadWrite(d, false, buf, n, off, NULL);
        if (NT_SUCCESS(st)) {
            crc = RtlCrc32(crc, buf, n);
            done += n;
        }
    }
    uint64_t ms = (KeQueryUptimeNs() - t0) / 1000000;
    if (NT_SUCCESS(st))
        ConPrint("crc32 %s: %08x over %llu bytes (%llu ms)\n", argv[1], crc, (unsigned long long)done,
                 (unsigned long long)ms);
    else
        ConPrint("crc32 %s: %s after %llu bytes\n", argv[1], RtlStatusName(st), (unsigned long long)done);
    if (buf)
        ExFreePoolWithTag(buf, 'KdSt');
    ObDereferenceObject(d);
}

/* The same xorshift32 pattern is generated by tools/test.sh to check the disk image. */
static void pattern(uint8_t *buf, uint32_t len, uint32_t *state)
{
    for (uint32_t i = 0; i < len; i += 4) {
        uint32_t x = *state;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        *state = x;
        memcpy(buf + i, &x, 4);
    }
}

void KdCmdWritetest(int argc, char **argv)
{
    if (argc < 5) {
        ConPrint("usage: writetest <device> <lba> <sector-count> <seed>\n");
        return;
    }
    DEVICE_OBJECT *d = open_dev(argv[1]);
    if (!d)
        return;
    uint32_t bps = d->SectorSize ? d->SectorSize : 512;
    uint64_t lba = parse_u64(argv[2]);
    uint32_t count = (uint32_t)parse_u64(argv[3]);
    uint32_t seed = (uint32_t)parse_u64(argv[4]);
    uint32_t len = count * bps;
    uint8_t *w = ExAllocatePoolWithTag(NonPagedPool, len, 'KdSt');
    uint8_t *r = ExAllocatePoolWithTag(NonPagedPool, len, 'KdSt');
    NTSTATUS st = (w && r) ? STATUS_SUCCESS : STATUS_NO_MEMORY;
    if (NT_SUCCESS(st)) {
        uint32_t state = seed ? seed : 1;
        pattern(w, len, &state);
        st = IoSynchronousReadWrite(d, true, w, len, lba * bps, NULL);
    }
    if (NT_SUCCESS(st))
        st = IoSynchronousFlush(d);
    if (NT_SUCCESS(st)) {
        memset(r, 0, len);
        st = IoSynchronousReadWrite(d, false, r, len, lba * bps, NULL);
    }
    if (NT_SUCCESS(st))
        ConPrint("writetest %s: %u sectors at LBA %llu %s\n", argv[1], count, (unsigned long long)lba,
                 memcmp(w, r, len) ? "MISMATCH after read-back" : "written and verified");
    else
        ConPrint("writetest %s: %s\n", argv[1], RtlStatusName(st));
    if (w) ExFreePoolWithTag(w, 'KdSt');
    if (r) ExFreePoolWithTag(r, 'KdSt');
    ObDereferenceObject(d);
}

void KdCmdCache(int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    CC_STATS s;
    CcQueryStats(&s);
    ConPrint("Block cache: %llu of %llu lines in use (%llu KiB), %llu dirty\n", (unsigned long long)s.Lines,
             (unsigned long long)s.CapacityLines, (unsigned long long)(s.Lines * 4), (unsigned long long)s.DirtyLines);
    ConPrint("  hits %llu, misses %llu, disk reads %llu, disk writes %llu, direct reads %llu, direct writes %llu\n",
             (unsigned long long)s.Hits, (unsigned long long)s.Misses, (unsigned long long)s.ReadsFromDisk,
             (unsigned long long)s.WritesToDisk, (unsigned long long)s.BypassReads, (unsigned long long)s.BypassWrites);
}

void KdCmdSync(int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    NTSTATUS st = CcFlushAll();
    for (unsigned i = 0; i < DiskCount(); i++)
        IoSynchronousFlush(DiskGet(i));
    ConPrint("sync: %s\n", NT_SUCCESS(st) ? "all disk caches written" : RtlStatusName(st));
}
