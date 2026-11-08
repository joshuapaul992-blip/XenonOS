/*
 * XenonOS kernel entry point.
 *
 * Phase 1.0 (bring-up): take over from Limine, set up the CPU tables, bring
 * up the serial and framebuffer consoles, print the version banner and a
 * short hardware summary, then halt.
 *
 * Kernel command line options (set in limine.conf):
 *   xenon.debugexit        exit QEMU through the isa-debug-exit device
 *                          instead of halting (used by tools/test.sh)
 *   xenon.crash=<kind>     crash on purpose to test the stop screen:
 *                          pagefault, divide, opcode, gpf, manual, assert
 */
#include "xenon.h"
#include "limine.h"
#include "hal.h"
#include "ke.h"
#include "rtl.h"
#include "console.h"
#include "init.h"
#include "../arch/x86_64/arch.h"
#include <xenon/version.h>

/* ---- Limine requests ---------------------------------------------------- */

SECTION(".limine_requests") USED static volatile LIMINE_BASE_REVISION(3);

SECTION(".limine_requests") USED static volatile struct limine_framebuffer_request g_fb_req = {
    .id = LIMINE_FRAMEBUFFER_REQUEST, .revision = 0 };
SECTION(".limine_requests") USED static volatile struct limine_memmap_request g_memmap_req = {
    .id = LIMINE_MEMMAP_REQUEST, .revision = 0 };
SECTION(".limine_requests") USED static volatile struct limine_hhdm_request g_hhdm_req = {
    .id = LIMINE_HHDM_REQUEST, .revision = 0 };
SECTION(".limine_requests") USED static volatile struct limine_bootloader_info_request g_bootinfo_req = {
    .id = LIMINE_BOOTLOADER_INFO_REQUEST, .revision = 0 };
SECTION(".limine_requests") USED static volatile struct limine_firmware_type_request g_fwtype_req = {
    .id = LIMINE_FIRMWARE_TYPE_REQUEST, .revision = 0 };
SECTION(".limine_requests") USED static volatile struct limine_executable_cmdline_request g_cmdline_req = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST, .revision = 0 };
SECTION(".limine_requests") USED static volatile struct limine_rsdp_request g_rsdp_req = {
    .id = LIMINE_RSDP_REQUEST, .revision = 0 };

SECTION(".limine_requests_start") USED static volatile LIMINE_REQUESTS_START_MARKER;
SECTION(".limine_requests_end") USED static volatile LIMINE_REQUESTS_END_MARKER;

/* ---- Helpers ------------------------------------------------------------ */

static bool g_test_mode;

bool KiTestMode(void)
{
    return g_test_mode;
}

const char *KiGetCommandLine(void)
{
    struct limine_executable_cmdline_response *r = g_cmdline_req.response;
    return (r && r->cmdline) ? r->cmdline : "";
}

static void print_banner(void)
{
    ConPrint("\x1b[1;97m" XENON_VERSION_STR "\x1b[0m\n");
    ConPrint("Copyright (c) 2026 XenonOS Project\n\n");
}

static void print_cpu(void)
{
    uint32_t a, b, c, d;
    char vendor[13];
    cpuid(0, 0, &a, &b, &c, &d);
    memcpy(vendor, &b, 4);
    memcpy(vendor + 4, &d, 4);
    memcpy(vendor + 8, &c, 4);
    vendor[12] = 0;

    char brand[49] = "";
    cpuid(0x80000000, 0, &a, &b, &c, &d);
    if (a >= 0x80000004) {
        uint32_t *p = (uint32_t *)brand;
        for (uint32_t leaf = 0x80000002; leaf <= 0x80000004; leaf++, p += 4)
            cpuid(leaf, 0, &p[0], &p[1], &p[2], &p[3]);
        brand[48] = 0;
    }
    const char *name = brand;
    while (*name == ' ')
        name++;
    DbgPrint("HAL: CPU %s, %s", vendor, *name ? name : "(no brand string)");
    DbgPrint("HAL: TSC %llu MHz", (unsigned long long)(HalGetTscHz() / 1000000));
}

static void print_memory(void)
{
    struct limine_memmap_response *mm = g_memmap_req.response;
    if (!mm) {
        DbgPrint("Mm: no memory map from the bootloader");
        return;
    }
    uint64_t usable = 0, reclaimable = 0, top = 0;
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE)
            usable += e->length;
        else if (e->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE || e->type == LIMINE_MEMMAP_ACPI_RECLAIMABLE)
            reclaimable += e->length;
        if (e->type == LIMINE_MEMMAP_USABLE || e->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE)
            top = MAX(top, e->base + e->length);
    }
    DbgPrint("Mm: %llu MiB usable, %llu KiB reclaimable, %llu map entries, top of RAM 0x%llx",
             (unsigned long long)(usable >> 20), (unsigned long long)(reclaimable >> 10),
             (unsigned long long)mm->entry_count, (unsigned long long)top);
}

static void maybe_crash(void)
{
    char kind[32];
    if (!RtlGetCmdlineOption(KiGetCommandLine(), "xenon.crash", kind, sizeof(kind)))
        return;
    DbgPrint("Ke: crashing on purpose (xenon.crash=%s)", kind);
    if (!strcmp(kind, "pagefault")) {
        volatile uint64_t *bad = (volatile uint64_t *)0xFFFF8FFFDEAD0000ull;
        *bad = 1;
    } else if (!strcmp(kind, "divide")) {
        /* Use the instruction directly: GCC turns "1 / x" into compares. */
        uint64_t zero = 0;
        __asm__ volatile("xor %%edx, %%edx; mov $1, %%eax; div %0" :: "r"(zero) : "rax", "rdx");
    } else if (!strcmp(kind, "opcode")) {
        __asm__ volatile("ud2");
    } else if (!strcmp(kind, "gpf")) {
        volatile uint64_t *noncanonical = (volatile uint64_t *)0x8000000000000000ull;
        *noncanonical = 1;
    } else if (!strcmp(kind, "assert")) {
        ASSERT(kind[0] == 0);
    } else {
        KeBugCheckEx(MANUALLY_INITIATED_CRASH, 0, 0, 0, 0);
    }
}

/* ---- Entry point -------------------------------------------------------- */

NORETURN void KiSystemStartup(void)
{
    HalSerialInit();
    if (!LIMINE_BASE_REVISION_SUPPORTED) {
        HalSerialWrite("XenonOS: bootloader does not support Limine base revision 3\n", 61);
        cpu_halt_forever();
    }

    g_test_mode = RtlGetCmdlineOption(KiGetCommandLine(), "xenon.debugexit", NULL, 0);

    struct limine_framebuffer_response *fbr = g_fb_req.response;
    if (fbr && fbr->framebuffer_count)
        FbConInit(fbr->framebuffers[0]);

    KiInitGdt();
    KiInitIdt();
    HalPicInitMasked();
    HalCalibrateTsc();

    print_banner();

    struct limine_bootloader_info_response *bi = g_bootinfo_req.response;
    struct limine_firmware_type_response *fw = g_fwtype_req.response;
    const char *fwname = "unknown firmware";
    if (fw)
        fwname = fw->firmware_type == LIMINE_FIRMWARE_TYPE_X86BIOS ? "BIOS"
               : fw->firmware_type == LIMINE_FIRMWARE_TYPE_UEFI64 ? "UEFI (64-bit)"
               : fw->firmware_type == LIMINE_FIRMWARE_TYPE_UEFI32 ? "UEFI (32-bit)" : "other firmware";
    DbgPrint("Boot: %s %s on %s", bi ? bi->name : "unknown bootloader", bi ? bi->version : "", fwname);
    DbgPrint("Boot: kernel built %s, command line \"%s\"", XENON_BUILD_DATE, KiGetCommandLine());

    print_cpu();
    print_memory();

    if (FbConPresent()) {
        unsigned w, h, fw_, fh, cols, rows;
        FbConGetResolution(&w, &h, &fw_, &fh);
        FbConGetSize(&cols, &rows);
        DbgPrint("Con: framebuffer %ux%u, font %ux%u, %ux%u text", w, h, fw_, fh, cols, rows);
    } else {
        DbgPrint("Con: no framebuffer, serial console only");
    }
    DbgPrint("Con: serial COM1 %s", HalSerialPresent() ? "present (115200 8N1)" : "not present");
    DbgPrint("HAL: ACPI RSDP %s, HHDM offset 0x%llx",
             g_rsdp_req.response ? "found" : "not found",
             (unsigned long long)(g_hhdm_req.response ? g_hhdm_req.response->offset : 0));

    maybe_crash();

    ConPrint("\nPhase 1.0 bring-up complete. The shell arrives in Phase 1.5.\n");
    ConPrint("System halted.\n");
    if (g_test_mode)
        HalDebugExit(XENON_EXIT_HALTED);
    cpu_halt_forever();
}
