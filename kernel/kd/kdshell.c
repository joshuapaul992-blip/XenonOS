/* SPDX-License-Identifier: MIT */
/*
 * Kd - kernel debug shell ("kd>").
 *
 * A small command line that runs as a kernel thread, for looking inside the
 * running kernel: the object namespace, threads, processes, memory, the
 * kernel log and interrupt counts. It reads from COM1 and the PS/2 keyboard.
 * The real user shell (C:/#) arrives in Phase 1.5.
 */
#include "ke.h"
#include "ob.h"
#include "ps.h"
#include "mm.h"
#include "ex.h"
#include "hal.h"
#include "rtl.h"
#include "console.h"
#include "../init/init.h"
#include <xenon/version.h>

void KdInitializeInput(void);
void KdCmdDisks(int, char **);
void KdCmdVolumes(int, char **);
void KdCmdHexdump(int, char **);
void KdCmdCrc32(int, char **);
void KdCmdWritetest(int, char **);
void KdCmdCache(int, char **);
void KdCmdSync(int, char **);
void IoShutdownSystem(void);
int KdGetChar(int64_t timeout_ns);

#define MAX_LINE 256
#define MAX_ARGS 8

/* ---- Line input ---------------------------------------------------------- */

static int read_line(char *buf, int size)
{
    int n = 0;
    for (;;) {
        int c = KdGetChar(KE_INFINITE);
        if (c == '\r' || c == '\n') {
            ConWrite("\n", 1);
            buf[n] = 0;
            return n;
        }
        if (c == 0x1b) {                     /* skip escape sequences (arrow keys) */
            int c2 = KdGetChar(-50 * 1000000ll);
            if (c2 == '[')
                while ((c2 = KdGetChar(-50 * 1000000ll)) >= 0 && !(c2 >= 0x40 && c2 <= 0x7E))
                    ;
            continue;
        }
        if (c == 0x08 || c == 0x7F) {
            if (n > 0) {
                n--;
                ConWrite("\b \b", 3);
            }
            continue;
        }
        if (c == 0x03) {                     /* Ctrl+C: drop the line */
            ConWrite("^C\n", 3);
            buf[0] = 0;
            return 0;
        }
        if (c == 0x15) {                     /* Ctrl+U: clear the line */
            while (n > 0) {
                n--;
                ConWrite("\b \b", 3);
            }
            continue;
        }
        if (c >= 0x20 && c < 0x7F && n < size - 1) {
            buf[n++] = (char)c;
            char ch = (char)c;
            ConWrite(&ch, 1);
        }
    }
}

static int split(char *line, char *argv[])
{
    int argc = 0;
    char *p = line;
    while (*p && argc < MAX_ARGS) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        argv[argc++] = p;
        while (*p && *p != ' ')
            p++;
        if (*p)
            *p++ = 0;
    }
    return argc;
}

/* ---- Commands -------------------------------------------------------------- */

static void cmd_help(int argc, char **argv);

static void cmd_ver(int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    ConPrint("%s, kernel built %s\n", XENON_VERSION_STR, XENON_BUILD_DATE);
}

typedef struct { unsigned Count; } OBJDIR_CTX;

static bool objdir_cb(void *obj, void *ctx)
{
    OBJDIR_CTX *c = ctx;
    OBJECT_TYPE *t = ObGetObjectType(obj);
    const char *target = ObGetSymbolicLinkTarget(obj);
    ConPrint("  %-24s %-14s", ObGetObjectName(obj), t ? t->Name : "?");
    if (target)
        ConPrint(" -> %s", target);
    else if (t == ObTypeObjectType)
        ConPrint(" %u objects, %u handles", ((OBJECT_TYPE *)obj)->TotalObjects, ((OBJECT_TYPE *)obj)->TotalHandles);
    ConPrint("\n");
    c->Count++;
    return true;
}

static void cmd_objdir(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "\\";
    void *obj;
    NTSTATUS st = ObReferenceObjectByName(path, 0, NULL, &obj);
    if (!NT_SUCCESS(st)) {
        ConPrint("objdir: %s: %s\n", path, RtlStatusName(st));
        return;
    }
    char full[256];
    ObQueryNameString(obj, full, sizeof(full));
    if (ObGetObjectType(obj) == ObDirectoryObjectType) {
        OBJDIR_CTX ctx = { 0 };
        ConPrint("Directory of %s\n", full[0] ? full : path);
        ObEnumerateDirectory(obj, objdir_cb, &ctx);
        ConPrint("  %u object%s\n", ctx.Count, ctx.Count == 1 ? "" : "s");
    } else {
        ConPrint("%s is a %s object (%lld references, %lld handles)\n", full, ObGetObjectType(obj)->Name,
                 (long long)ObGetPointerCount(obj) - 1, (long long)ObGetHandleCount(obj));
    }
    ObDereferenceObject(obj);
}

static void thread_cb(KTHREAD *t, void *ctx)
{
    UNUSED(ctx);
    ETHREAD *et = (ETHREAD *)t;
    ConPrint("%5llu %5llu %-10s %3d %9llu %8llu  %s\n", (unsigned long long)t->Cid,
             (unsigned long long)(et->ThreadsProcess ? et->ThreadsProcess->UniqueProcessId : 0),
             KeThreadStateName(t->State), t->Priority, (unsigned long long)(t->KernelTime * 1000 / KE_CLOCK_HZ),
             (unsigned long long)t->ContextSwitches, t->Name);
}

static void cmd_threads(int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    ConPrint("  TID   PID State      Pri   CPU(ms) Switches  Name\n");
    KeEnumerateThreads(thread_cb, NULL);
    ConPrint("%u threads, %llu context switches\n", KeThreadCount(), (unsigned long long)KeQueryContextSwitches());
}

static void process_cb(EPROCESS *p, void *ctx)
{
    UNUSED(ctx);
    ConPrint("%5llu %7u %7u  %s\n", (unsigned long long)p->UniqueProcessId, p->ActiveThreads,
             p->HandleTable.Count, p->ImageFileName);
}

static void cmd_ps(int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    ConPrint("  PID Threads Handles  Name\n");
    PsEnumerateProcesses(process_cb, NULL);
}

static void tag_string(uint32_t tag, char out[5])
{
    for (int i = 0; i < 4; i++) {
        char c = (char)(tag >> (24 - 8 * i));
        out[i] = (c >= 0x20 && c < 0x7F) ? c : '.';
    }
    out[4] = 0;
}

static void cmd_mem(int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    MM_STATS m;
    POOL_STATS p;
    MmQueryStats(&m);
    ExQueryPoolStats(&p);
    ConPrint("Physical memory: %llu KiB total, %llu KiB free, %llu KiB used\n",
             (unsigned long long)(m.TotalPages * 4), (unsigned long long)(m.FreePages * 4),
             (unsigned long long)((m.TotalPages - m.FreePages) * 4));
    ConPrint("Kernel pool:     %llu KiB in %llu allocations (%llu pages), %llu allocs / %llu frees so far\n",
             (unsigned long long)(p.LiveBytes / 1024), (unsigned long long)p.LiveAllocations,
             (unsigned long long)p.PagesInPool, (unsigned long long)p.TotalAllocations,
             (unsigned long long)p.TotalFrees);
    POOL_TAG_INFO tags[64];
    unsigned n = ExQueryPoolTags(tags, 64);
    ConPrint("  Tag   Allocs     Bytes\n");
    for (unsigned i = 0; i < n && i < 64; i++) {
        char t[5];
        tag_string(tags[i].Tag, t);
        ConPrint("  %s %7u %9llu\n", t, tags[i].Allocations, (unsigned long long)tags[i].Bytes);
    }
}

static void cmd_dmesg(int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    char *buf = ExAllocatePoolWithTag(NonPagedPool, EX_LOG_SIZE, 'KdSh');
    if (!buf) {
        ConPrint("dmesg: out of memory\n");
        return;
    }
    size_t n = ExLogRead(buf, EX_LOG_SIZE);
    for (size_t i = 0; i < n; i += 256)
        ConWrite(buf + i, MIN(256, n - i));
    ExFreePoolWithTag(buf, 'KdSh');
}

static void cmd_uptime(int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    uint64_t ns = KeQueryUptimeNs();
    uint64_t s = ns / 1000000000ull;
    uint64_t idle = KeQueryIdleTicks() * 1000 / KE_CLOCK_HZ, total = KeTickCount * 1000 / KE_CLOCK_HZ;
    ConPrint("up %llu:%02llu:%02llu.%03llu, idle %llu%%, %llu clock ticks, %llu context switches\n",
             (unsigned long long)(s / 3600), (unsigned long long)(s / 60 % 60), (unsigned long long)(s % 60),
             (unsigned long long)(ns / 1000000 % 1000), (unsigned long long)(total ? idle * 100 / total : 0),
             (unsigned long long)KeTickCount, (unsigned long long)KeQueryContextSwitches());
}

static void cmd_date(int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    static const char *days[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    TIME_FIELDS t;
    if (HalQueryRealTimeClock(&t))
        ConPrint("%s %04u-%02u-%02u %02u:%02u:%02u UTC\n", days[t.Weekday], t.Year, t.Month, t.Day, t.Hour,
                 t.Minute, t.Second);
    else
        ConPrint("date: no real-time clock\n");
}

static void cmd_irq(int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    ConPrint("Vector IRQL      Count\n");
    for (unsigned v = 0x20; v < 256; v++) {
        uint64_t c = KeQueryInterruptCount((uint8_t)v);
        if (c)
            ConPrint("  0x%02x  %4u %10llu%s\n", v, v >> 4, (unsigned long long)c,
                     v == VECTOR_CLOCK ? "  clock" : v == VECTOR_SPURIOUS ? "  spurious" : "");
    }
}

static void cmd_pci(int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    for (unsigned i = 0; i < HalPciDeviceCount(); i++) {
        HAL_PCI_DEVICE *d = HalPciGetDevice(i);
        const char *v = HalPciVendorName(d->VendorId);
        ConPrint("%02x:%02x.%u %04x:%04x %s%s%s\n", d->Bus, d->Device, d->Function, d->VendorId, d->DeviceId,
                 v ? v : "", v ? " " : "", HalPciClassName(d->ClassCode, d->SubClass, d->ProgIf));
    }
}

/* threadtest: start N threads that sleep and take a shared mutex. */
static KMUTEX g_tt_mutex;
static volatile unsigned g_tt_count;
static void tt_worker(void *ctx)
{
    unsigned seed = (unsigned)(uintptr_t)ctx * 2654435761u;
    for (int i = 0; i < 10; i++) {
        seed = seed * 1103515245u + 12345u;
        KeSleepMs(1 + (seed >> 16) % 10);
        KeWaitForSingleObject(&g_tt_mutex, KE_INFINITE);
        g_tt_count++;
        KeReleaseMutex(&g_tt_mutex);
    }
}

static void cmd_threadtest(int argc, char **argv)
{
    unsigned n = argc > 1 ? (unsigned)atoi(argv[1]) : 50;
    if (n == 0 || n > 500) n = 50;
    KeInitializeMutex(&g_tt_mutex);
    g_tt_count = 0;
    HANDLE *h = ExAllocatePoolWithTag(NonPagedPoolZeroed, n * sizeof(HANDLE), 'KdSh');
    uint64_t t0 = KeQueryUptimeNs();
    unsigned started = 0;
    for (unsigned i = 0; i < n; i++)
        if (NT_SUCCESS(PsCreateSystemThread(&h[i], tt_worker, (void *)(uintptr_t)(i + 1), 8, "ThreadTest", NULL)))
            started++;
    for (unsigned i = 0; i < n; i++) {
        if (!h[i]) continue;
        void *t;
        if (NT_SUCCESS(ObReferenceObjectByHandle(h[i], PsThreadType, &t))) {
            KeWaitForSingleObject(t, KE_INFINITE);
            ObDereferenceObject(t);
        }
        ObCloseHandle(h[i]);
    }
    ExFreePoolWithTag(h, 'KdSh');
    ConPrint("threadtest: %u threads finished, %u mutex acquisitions (expected %u), %llu ms\n", started,
             g_tt_count, started * 10, (unsigned long long)((KeQueryUptimeNs() - t0) / 1000000));
}

static void cmd_sleep(int argc, char **argv)
{
    unsigned ms = argc > 1 ? (unsigned)atoi(argv[1]) : 1000;
    uint64_t t0 = KeQueryUptimeNs();
    KeSleepMs(ms);
    ConPrint("slept %llu ms\n", (unsigned long long)((KeQueryUptimeNs() - t0) / 1000000));
}

static void cmd_crash(int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    KeBugCheckEx(MANUALLY_INITIATED_CRASH, 0, 0, 0, 0);
}

static void cmd_reboot(int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    IoShutdownSystem();
    ConPrint("Rebooting...\n");
    HalReboot();
}

static void cmd_exit(int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    if (KiTestMode()) {
        IoShutdownSystem();
        ConPrint("Leaving the test run.\n");
        HalDebugExit(XENON_EXIT_HALTED);
    }
    ConPrint("exit only works in test runs (xenon.debugexit); use 'reboot'.\n");
}

static void cmd_clear(int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    ConWrite("\x1b[2J\x1b[H", 7);
}

static const struct {
    const char *Name;
    void (*Fn)(int, char **);
    const char *Help;
} g_cmds[] = {
    { "help",       cmd_help,       "list commands" },
    { "ver",        cmd_ver,        "show the version" },
    { "objdir",     cmd_objdir,     "objdir [path]  list an object directory (default \\)" },
    { "threads",    cmd_threads,    "list threads" },
    { "ps",         cmd_ps,         "list processes" },
    { "mem",        cmd_mem,        "memory and pool usage by tag" },
    { "dmesg",      cmd_dmesg,      "show the kernel log" },
    { "uptime",     cmd_uptime,     "time since boot and idle time" },
    { "date",       cmd_date,       "read the real-time clock" },
    { "irq",        cmd_irq,        "interrupt counts per vector" },
    { "pci",        cmd_pci,        "list PCI devices" },
    { "disks",      KdCmdDisks,     "list disks and floppy drives" },
    { "volumes",    KdCmdVolumes,   "list partitions (volume devices)" },
    { "hexdump",    KdCmdHexdump,   "hexdump <device> [lba]  show one sector" },
    { "crc32",      KdCmdCrc32,     "crc32 <device> [lba [count]]  checksum sectors" },
    { "writetest",  KdCmdWritetest, "writetest <device> <lba> <count> <seed>" },
    { "cache",      KdCmdCache,     "block cache statistics" },
    { "sync",       KdCmdSync,      "write all cached disk data" },
    { "threadtest", cmd_threadtest, "threadtest [n]  run n threads that share a mutex" },
    { "sleep",      cmd_sleep,      "sleep [ms]" },
    { "clear",      cmd_clear,      "clear the screen" },
    { "crash",      cmd_crash,      "stop the system (test the stop screen)" },
    { "reboot",     cmd_reboot,     "restart the computer" },
    { "exit",       cmd_exit,       "end a test run" },
};

static void cmd_help(int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    ConPrint("Kernel debug shell commands:\n");
    for (unsigned i = 0; i < ARRAY_SIZE(g_cmds); i++)
        ConPrint("  %-11s %s\n", g_cmds[i].Name, g_cmds[i].Help);
}

static void shell_thread(void *ctx)
{
    UNUSED(ctx);
    KdInitializeInput();
    ConPrint("\nXenonOS kernel debug shell. Type 'help' for commands.\n");
    char line[MAX_LINE];
    char *argv[MAX_ARGS];
    for (;;) {
        ConPrint("kd> ");
        read_line(line, sizeof(line));
        int argc = split(line, argv);
        if (argc == 0)
            continue;
        bool found = false;
        for (unsigned i = 0; i < ARRAY_SIZE(g_cmds); i++) {
            if (!strcasecmp(argv[0], g_cmds[i].Name)) {
                g_cmds[i].Fn(argc, argv);
                found = true;
                break;
            }
        }
        if (!found)
            ConPrint("%s: unknown command (try 'help')\n", argv[0]);
    }
}

void KdStartShell(void)
{
    if (!NT_SUCCESS(PsCreateSystemThread(NULL, shell_thread, NULL, 9, "KdShell", NULL)))
        DbgPrint("Kd: could not start the debug shell");
}
