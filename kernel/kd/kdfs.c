/* SPDX-License-Identifier: MIT */
/*
 * Kd - file system commands for the kernel debug shell (until the real
 * shell arrives in Phase 1.5). Paths are absolute: C:\dir\file or C:/dir/file.
 */
#include "io.h"
#include "hal.h"
#include "mm.h"
#include "rtl.h"
#include "console.h"

#define BUF (64 * 1024)

static void fail(const char *what, const char *path, NTSTATUS st)
{
    ConPrint("%s %s: %s\n", what, path, RtlStatusName(st));
}

static void size_string(uint64_t b, char *out, size_t n)
{
    if (b >= (10ull << 30)) snprintf(out, n, "%llu GiB", (unsigned long long)(b >> 30));
    else if (b >= (10ull << 20)) snprintf(out, n, "%llu MiB", (unsigned long long)(b >> 20));
    else snprintf(out, n, "%llu KiB", (unsigned long long)(b >> 10));
}

void KdCmdDrives(int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    ConPrint("Drive  FS     Label        Serial     Size       Free       Device\n");
    for (char c = 'A'; c <= 'Z'; c++) {
        MOUNT_INFO mi;
        if (!MountMgrQuery(c, &mi))
            continue;
        char total[24] = "-", freeb[24] = "-", serial[16] = "-";
        if (mi.MediaPresent) {
            size_string(mi.TotalBytes, total, sizeof(total));
            size_string(mi.FreeBytes, freeb, sizeof(freeb));
            snprintf(serial, sizeof(serial), "%04X-%04X", mi.Serial >> 16, mi.Serial & 0xFFFF);
        }
        ConPrint("%c:     %-6s %-12s %-10s %-10s %-10s %s%s%s\n", c, mi.MediaPresent ? mi.FileSystem : "-",
                 mi.Label, serial, mi.MediaPresent ? total : (mi.Floppy ? "no disk" : "-"), freeb, mi.Device,
                 mi.Boot ? " (boot)" : "", mi.Removable && !mi.Floppy ? " (removable)" : "");
    }
}

void KdCmdDir(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "C:\\";
    const char *pattern = argc > 2 ? argv[2] : "*";
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    NTSTATUS st = NtCreateFile(&h, FILE_READ_DATA, path, &iosb, 0, FILE_OPEN, FILE_DIRECTORY_FILE);
    if (!NT_SUCCESS(st)) {
        fail("dir", path, st);
        return;
    }
    uint8_t *buf = ExAllocatePoolWithTag(NonPagedPool, 8192, 'KdFs');
    unsigned files = 0, dirs = 0;
    uint64_t bytes = 0;
    ConPrint("Directory of %s\n", path);
    bool restart = true;
    while (buf) {
        st = NtQueryDirectoryFile(h, &iosb, buf, 8192, FileDirectoryInformation, false, pattern, restart);
        restart = false;
        if (!NT_SUCCESS(st))
            break;
        FILE_DIRECTORY_INFORMATION *d = (FILE_DIRECTORY_INFORMATION *)buf;
        for (;;) {
            TIME_FIELDS t;
            RtlUnixToTimeFields(RtlNtTimeToUnix(d->LastWriteTime), &t);
            bool isdir = d->FileAttributes & FILE_ATTRIBUTE_DIRECTORY;
            char attr[5] = "----";
            if (d->FileAttributes & FILE_ATTRIBUTE_READONLY) attr[0] = 'r';
            if (d->FileAttributes & FILE_ATTRIBUTE_HIDDEN) attr[1] = 'h';
            if (d->FileAttributes & FILE_ATTRIBUTE_SYSTEM) attr[2] = 's';
            if (d->FileAttributes & FILE_ATTRIBUTE_ARCHIVE) attr[3] = 'a';
            if (isdir)
                ConPrint("%04u-%02u-%02u %02u:%02u  %s  <DIR>       %-12s %s\n", t.Year, t.Month, t.Day, t.Hour,
                         t.Minute, attr, d->ShortName, d->FileName);
            else
                ConPrint("%04u-%02u-%02u %02u:%02u  %s  %10lld  %-12s %s\n", t.Year, t.Month, t.Day, t.Hour,
                         t.Minute, attr, (long long)d->EndOfFile, d->ShortName, d->FileName);
            if (isdir) dirs++; else { files++; bytes += (uint64_t)d->EndOfFile; }
            if (!d->NextEntryOffset)
                break;
            d = (FILE_DIRECTORY_INFORMATION *)((uint8_t *)d + d->NextEntryOffset);
        }
    }
    ConPrint("  %u file(s), %llu bytes, %u dir(s)\n", files, (unsigned long long)bytes, dirs);
    if (buf)
        ExFreePoolWithTag(buf, 'KdFs');
    NtClose(h);
}

static NTSTATUS open_read(const char *path, HANDLE *h)
{
    IO_STATUS_BLOCK iosb;
    return NtCreateFile(h, FILE_READ_DATA, path, &iosb, 0, FILE_OPEN, FILE_NON_DIRECTORY_FILE);
}

void KdCmdType(int argc, char **argv)
{
    if (argc < 2) { ConPrint("usage: type <file>\n"); return; }
    HANDLE h;
    NTSTATUS st = open_read(argv[1], &h);
    if (!NT_SUCCESS(st)) { fail("type", argv[1], st); return; }
    char *buf = ExAllocatePoolWithTag(NonPagedPool, 4096, 'KdFs');
    IO_STATUS_BLOCK iosb;
    while (buf && NT_SUCCESS(NtReadFile(h, buf, 4096, NULL, &iosb)) && iosb.Information)
        ConWrite(buf, iosb.Information);
    ConWrite("\n", 1);
    if (buf) ExFreePoolWithTag(buf, 'KdFs');
    NtClose(h);
}

void KdCmdFcrc(int argc, char **argv)
{
    if (argc < 2) { ConPrint("usage: fcrc <file>\n"); return; }
    HANDLE h;
    NTSTATUS st = open_read(argv[1], &h);
    if (!NT_SUCCESS(st)) { fail("fcrc", argv[1], st); return; }
    uint8_t *buf = ExAllocatePoolWithTag(NonPagedPool, BUF, 'KdFs');
    IO_STATUS_BLOCK iosb;
    uint32_t crc = 0;
    uint64_t total = 0;
    while (buf) {
        st = NtReadFile(h, buf, BUF, NULL, &iosb);
        if (st == STATUS_END_OF_FILE || (NT_SUCCESS(st) && !iosb.Information)) { st = STATUS_SUCCESS; break; }
        if (!NT_SUCCESS(st)) break;
        crc = RtlCrc32(crc, buf, iosb.Information);
        total += iosb.Information;
    }
    if (NT_SUCCESS(st))
        ConPrint("fcrc %s: %08x over %llu bytes\n", argv[1], crc, (unsigned long long)total);
    else
        fail("fcrc", argv[1], st);
    if (buf) ExFreePoolWithTag(buf, 'KdFs');
    NtClose(h);
}

void KdCmdMkdir(int argc, char **argv)
{
    if (argc < 2) { ConPrint("usage: mkdir <dir>\n"); return; }
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    NTSTATUS st = NtCreateFile(&h, FILE_READ_DATA, argv[1], &iosb, 0, FILE_CREATE, FILE_DIRECTORY_FILE);
    if (!NT_SUCCESS(st)) { fail("mkdir", argv[1], st); return; }
    NtClose(h);
    ConPrint("mkdir %s: ok\n", argv[1]);
}

void KdCmdDel(int argc, char **argv)
{
    if (argc < 2) { ConPrint("usage: del <file-or-empty-dir>\n"); return; }
    NTSTATUS st = NtDeleteFile(argv[1]);
    if (!NT_SUCCESS(st)) { fail("del", argv[1], st); return; }
    ConPrint("del %s: ok\n", argv[1]);
}

void KdCmdRen(int argc, char **argv)
{
    if (argc < 3) { ConPrint("usage: ren <old-path> <new-path>\n"); return; }
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    NTSTATUS st = NtCreateFile(&h, DELETE, argv[1], &iosb, 0, FILE_OPEN, 0);
    if (NT_SUCCESS(st)) {
        st = NtSetInformationFile(h, &iosb, argv[2], (uint32_t)strlen(argv[2]) + 1, FileRenameInformation);
        NtClose(h);
    }
    if (!NT_SUCCESS(st)) { fail("ren", argv[1], st); return; }
    ConPrint("ren %s -> %s: ok\n", argv[1], argv[2]);
}

void KdCmdCopy(int argc, char **argv)
{
    if (argc < 3) { ConPrint("usage: copy <from> <to>\n"); return; }
    HANDLE in, out;
    IO_STATUS_BLOCK iosb;
    NTSTATUS st = open_read(argv[1], &in);
    if (!NT_SUCCESS(st)) { fail("copy", argv[1], st); return; }
    st = NtCreateFile(&out, FILE_WRITE_DATA, argv[2], &iosb, 0, FILE_OVERWRITE_IF, FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(st)) { fail("copy", argv[2], st); NtClose(in); return; }
    uint8_t *buf = ExAllocatePoolWithTag(NonPagedPool, BUF, 'KdFs');
    uint64_t total = 0;
    while (buf) {
        st = NtReadFile(in, buf, BUF, NULL, &iosb);
        if (st == STATUS_END_OF_FILE || (NT_SUCCESS(st) && !iosb.Information)) { st = STATUS_SUCCESS; break; }
        if (!NT_SUCCESS(st)) break;
        uint64_t n = iosb.Information;
        st = NtWriteFile(out, buf, (uint32_t)n, NULL, &iosb);
        if (!NT_SUCCESS(st)) break;
        total += n;
    }
    if (buf) ExFreePoolWithTag(buf, 'KdFs');
    NtClose(in);
    NtClose(out);
    if (NT_SUCCESS(st)) ConPrint("copy: %llu bytes copied to %s\n", (unsigned long long)total, argv[2]);
    else fail("copy", argv[2], st);
}

/* write <file> <size> <seed> [append]: the same xorshift32 pattern as writetest. */
void KdCmdWrite(int argc, char **argv)
{
    if (argc < 4) { ConPrint("usage: write <file> <bytes> <seed> [append]\n"); return; }
    uint64_t size = 0;
    for (const char *p = argv[2]; *p >= '0' && *p <= '9'; p++) size = size * 10 + (uint64_t)(*p - '0');
    uint32_t state = 0;
    for (const char *p = argv[3]; *p >= '0' && *p <= '9'; p++) state = state * 10 + (uint32_t)(*p - '0');
    if (!state) state = 1;
    bool append = argc > 4 && !strcmp(argv[4], "append");
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    NTSTATUS st = NtCreateFile(&h, FILE_WRITE_DATA | FILE_APPEND_DATA, argv[1], &iosb, 0,
                               append ? FILE_OPEN_IF : FILE_OVERWRITE_IF, FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(st)) { fail("write", argv[1], st); return; }
    uint8_t *buf = ExAllocatePoolWithTag(NonPagedPool, BUF, 'KdFs');
    uint64_t done = 0;
    int64_t at_end = -1;
    while (buf && done < size) {
        uint32_t n = (uint32_t)MIN((uint64_t)BUF, size - done);
        for (uint32_t i = 0; i < n; i += 4) {
            state ^= state << 13; state ^= state >> 17; state ^= state << 5;
            memcpy(buf + i, &state, MIN(4u, n - i));
        }
        st = NtWriteFile(h, buf, n, append ? &at_end : NULL, &iosb);
        if (!NT_SUCCESS(st)) break;
        done += n;
    }
    if (buf) ExFreePoolWithTag(buf, 'KdFs');
    NtClose(h);
    if (NT_SUCCESS(st)) ConPrint("write %s: %llu bytes\n", argv[1], (unsigned long long)done);
    else fail("write", argv[1], st);
}

void KdCmdAttrib(int argc, char **argv)
{
    if (argc < 2) { ConPrint("usage: attrib <path> [+r|-r|+h|-h|+s|-s|+a|-a ...]\n"); return; }
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    NTSTATUS st = NtCreateFile(&h, FILE_READ_DATA, argv[1], &iosb, 0, FILE_OPEN, 0);
    if (!NT_SUCCESS(st)) { fail("attrib", argv[1], st); return; }
    FILE_BASIC_INFORMATION b;
    st = NtQueryInformationFile(h, &iosb, &b, sizeof(b), FileBasicInformation);
    if (NT_SUCCESS(st) && argc > 2) {
        uint32_t a = b.FileAttributes == FILE_ATTRIBUTE_NORMAL ? 0 : b.FileAttributes;
        for (int i = 2; i < argc; i++) {
            uint32_t bit = 0;
            switch (tolower(argv[i][1])) {
            case 'r': bit = FILE_ATTRIBUTE_READONLY; break;
            case 'h': bit = FILE_ATTRIBUTE_HIDDEN; break;
            case 's': bit = FILE_ATTRIBUTE_SYSTEM; break;
            case 'a': bit = FILE_ATTRIBUTE_ARCHIVE; break;
            }
            if (argv[i][0] == '+') a |= bit; else a &= ~bit;
        }
        FILE_BASIC_INFORMATION nb = { 0 };
        nb.FileAttributes = (a & ~FILE_ATTRIBUTE_DIRECTORY) ? (a & ~FILE_ATTRIBUTE_DIRECTORY) : FILE_ATTRIBUTE_NORMAL;
        st = NtSetInformationFile(h, &iosb, &nb, sizeof(nb), FileBasicInformation);
        if (NT_SUCCESS(st))
            st = NtQueryInformationFile(h, &iosb, &b, sizeof(b), FileBasicInformation);
    }
    NtClose(h);
    if (!NT_SUCCESS(st)) { fail("attrib", argv[1], st); return; }
    uint32_t a = b.FileAttributes;
    ConPrint("attrib %s: %c%c%c%c%s\n", argv[1], a & FILE_ATTRIBUTE_READONLY ? 'R' : '-',
             a & FILE_ATTRIBUTE_HIDDEN ? 'H' : '-', a & FILE_ATTRIBUTE_SYSTEM ? 'S' : '-',
             a & FILE_ATTRIBUTE_ARCHIVE ? 'A' : '-', a & FILE_ATTRIBUTE_DIRECTORY ? " (directory)" : "");
}

void KdCmdMount(int argc, char **argv)
{
    if (argc < 3) {
        KdCmdDrives(0, NULL);
        return;
    }
    NTSTATUS st = MountMgrAssign(argv[1][0], argv[2]);
    ConPrint("mount %c: %s: %s\n", toupper(argv[1][0]), argv[2], NT_SUCCESS(st) ? "ok" : RtlStatusName(st));
}

void KdCmdUmount(int argc, char **argv)
{
    if (argc < 2) { ConPrint("usage: umount <X:>\n"); return; }
    NTSTATUS st = MountMgrRemove(argv[1][0]);
    ConPrint("umount %c: %s\n", toupper(argv[1][0]), NT_SUCCESS(st) ? "ok" : RtlStatusName(st));
}
