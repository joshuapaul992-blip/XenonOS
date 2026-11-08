/*
 * XenonOS kernel entry point.
 *
 * Boot runs in two stages, like NT's phase 0 / phase 1:
 *
 *   KiSystemStartup (on Limine's stack, Limine's page tables)
 *     - consoles, CPU tables, early TSC timestamps, banner
 *     - copy everything needed from Limine into KeLoaderBlock
 *     - build the kernel's own page tables and switch to them
 *     - switch to a kernel stack with a guard page
 *
 *   KiSystemStartupPhase1 (on the kernel stack)
 *     - give Limine's memory back to the page allocator
 *     - pool, object manager, processes and the scheduler (this code
 *       becomes the "Init" thread)
 *     - ACPI, APIC/IOAPIC, clock tick, RTC, PCI
 *     - self-tests when requested, then start the debug shell and exit
 *
 * Kernel command line options (set in limine.conf):
 *   xenon.debugexit        exit QEMU through the isa-debug-exit device
 *                          instead of halting (used by tools/test.sh)
 *   xenon.selftest         run the kernel self-tests after boot
 *   xenon.interactive      with xenon.debugexit: start the debug shell
 *                          instead of exiting (tests drive it over COM1)
 *   xenon.crash=<kind>     crash on purpose to test the stop screen:
 *                          pagefault, divide, opcode, gpf, manual, assert,
 *                          stackoverflow, doublefree
 */
#include "xenon.h"
#include "limine.h"
#include "hal.h"
#include "ke.h"
#include "mm.h"
#include "rtl.h"
#include "console.h"
#include "loader.h"
#include "ob.h"
#include "ps.h"
#include "init.h"
#include "../arch/x86_64/arch.h"
#include <xenon/version.h>

#define KERNEL_STACK_SIZE (64 * 1024)

void KiRunSelfTests(void);
void KdStartShell(void);
static void *g_boot_stack;
NORETURN void KiSystemStartup(void);

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
SECTION(".limine_requests") USED static volatile struct limine_kernel_address_request g_kaddr_req = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST, .revision = 0 };

SECTION(".limine_requests_start") USED static volatile LIMINE_REQUESTS_START_MARKER;
SECTION(".limine_requests_end") USED static volatile LIMINE_REQUESTS_END_MARKER;

/* ---- Loader block ------------------------------------------------------- */

LOADER_BLOCK KeLoaderBlock;
static bool g_test_mode;

bool KiTestMode(void)
{
    return g_test_mode;
}

const char *KiGetCommandLine(void)
{
    return KeLoaderBlock.CommandLine;
}

static void capture_loader_block(void)
{
    LOADER_BLOCK *lb = &KeLoaderBlock;

    if (!g_hhdm_req.response || !g_memmap_req.response || !g_kaddr_req.response) {
        HalSerialWrite("XenonOS: bootloader did not provide HHDM, memory map or kernel address\n", 72);
        cpu_halt_forever();
    }
    lb->HhdmOffset = g_hhdm_req.response->offset;
    lb->KernelPhysicalBase = g_kaddr_req.response->physical_base;
    lb->KernelVirtualBase = g_kaddr_req.response->virtual_base;

    struct limine_memmap_response *mm = g_memmap_req.response;
    for (uint64_t i = 0; i < mm->entry_count && lb->MemoryMapCount < LOADER_MAX_MEMMAP; i++) {
        lb->MemoryMap[lb->MemoryMapCount++] = (LOADER_MEMORY_DESCRIPTOR){
            .Base = mm->entries[i]->base, .Length = mm->entries[i]->length, .Type = (uint32_t)mm->entries[i]->type };
    }

    if (g_rsdp_req.response && g_rsdp_req.response->address) {
        /* Base revision 3 gives a physical address; older ones a virtual one. */
        uint64_t a = (uint64_t)g_rsdp_req.response->address;
        lb->RsdpPhysical = a >= lb->HhdmOffset ? a - lb->HhdmOffset : a;
    }

    struct limine_framebuffer_response *fbr = g_fb_req.response;
    if (fbr && fbr->framebuffer_count) {
        struct limine_framebuffer *f = fbr->framebuffers[0];
        lb->HaveFramebuffer = true;
        lb->Framebuffer = (LOADER_FRAMEBUFFER){
            .PhysicalAddress = (uint64_t)f->address - lb->HhdmOffset,
            .Width = f->width, .Height = f->height, .Pitch = f->pitch, .Bpp = f->bpp,
            .MemoryModel = f->memory_model,
            .RedSize = f->red_mask_size, .RedShift = f->red_mask_shift,
            .GreenSize = f->green_mask_size, .GreenShift = f->green_mask_shift,
            .BlueSize = f->blue_mask_size, .BlueShift = f->blue_mask_shift,
        };
    }

    struct limine_executable_cmdline_response *cl = g_cmdline_req.response;
    if (cl && cl->cmdline)
        strlcpy(lb->CommandLine, cl->cmdline, sizeof(lb->CommandLine));
    struct limine_bootloader_info_response *bi = g_bootinfo_req.response;
    strlcpy(lb->BootloaderName, bi ? bi->name : "unknown bootloader", sizeof(lb->BootloaderName));
    strlcpy(lb->BootloaderVersion, bi ? bi->version : "", sizeof(lb->BootloaderVersion));

    lb->FirmwareName = "unknown firmware";
    if (g_fwtype_req.response) {
        switch (g_fwtype_req.response->firmware_type) {
        case LIMINE_FIRMWARE_TYPE_X86BIOS: lb->FirmwareName = "BIOS"; break;
        case LIMINE_FIRMWARE_TYPE_UEFI64:  lb->FirmwareName = "UEFI (64-bit)"; lb->Uefi = true; break;
        case LIMINE_FIRMWARE_TYPE_UEFI32:  lb->FirmwareName = "UEFI (32-bit)"; lb->Uefi = true; break;
        default: lb->FirmwareName = "other firmware"; break;
        }
    }
}

/* ---- Boot messages ------------------------------------------------------ */

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
}

static void print_memory(void)
{
    MM_STATS st;
    MmQueryStats(&st);
    DbgPrint("Mm: %llu MiB RAM, %llu MiB free, kernel image %llu KiB, %llu KiB reclaimed from the bootloader",
             (unsigned long long)(st.TotalPages * PAGE_SIZE >> 20),
             (unsigned long long)(st.FreePages * PAGE_SIZE >> 20),
             (unsigned long long)(st.KernelImagePages * PAGE_SIZE >> 10),
             (unsigned long long)(st.ReclaimedPages * PAGE_SIZE >> 10));
}

static const char *floppy_type(uint8_t t)
{
    switch (t) {
    case 0: return "none";
    case 1: return "5.25\" 360 KB";
    case 2: return "5.25\" 1.2 MB";
    case 3: return "3.5\" 720 KB";
    case 4: return "3.5\" 1.44 MB";
    case 5: return "3.5\" 2.88 MB";
    default: return "unknown";
    }
}

static void print_platform(void)
{
    DbgPrint("HAL: %u CPU%s present (1 online; SMP comes in a later milestone), %s legacy PIC",
             HalAcpi.CpuCount ? HalAcpi.CpuCount : 1, HalAcpi.CpuCount == 1 ? "" : "s",
             HalAcpi.HasLegacyPic ? "with" : "without");

    TIME_FIELDS t;
    static const char *days[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    if (HalQueryRealTimeClock(&t))
        DbgPrint("HAL: RTC %s %04u-%02u-%02u %02u:%02u:%02u UTC", days[t.Weekday], t.Year, t.Month, t.Day,
                 t.Hour, t.Minute, t.Second);
    else
        DbgPrint("HAL: RTC not available");

    uint8_t fd = HalCmosRead(0x10);
    DbgPrint("HAL: CMOS floppy drives: A: %s, B: %s", floppy_type(fd >> 4), floppy_type(fd & 0xF));
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
    } else if (!strcmp(kind, "stackoverflow")) {
        /* Run off the bottom of the kernel stack into its guard page. */
        __asm__ volatile("1: push $0; jmp 1b");
    } else if (!strcmp(kind, "doublefree")) {
        void *p = ExAllocatePoolWithTag(NonPagedPool, 100, 'Test');
        ExFreePoolWithTag(p, 'Test');
        ExFreePoolWithTag(p, 'Test');
    } else if (!strcmp(kind, "writecode")) {
        /* Kernel code is mapped read-only. */
        *(volatile uint8_t *)(uintptr_t)&KiSystemStartup = 0xCC;
    } else {
        KeBugCheckEx(MANUALLY_INITIATED_CRASH, 0, 0, 0, 0);
    }
}

/* ---- Phase 1 ------------------------------------------------------------ */

NORETURN static void KiSystemStartupPhase1(void)
{
    MmReclaimBootloaderMemory();
    ExInitializePool();
    print_memory();

    ObInitSystem();
    PsInitSystem(g_boot_stack, KERNEL_STACK_SIZE);
    DbgPrint("Ke: scheduler running: %u priorities, %u ms time slice, %u Hz clock", MAXIMUM_PRIORITY,
             THREAD_QUANTUM_MS, KE_CLOCK_HZ);

    HalInitAcpi();
    print_cpu();
    HalInitApic();
    HalInitClock();
    cpu_sti();
    print_platform();
    HalInitPci();

    maybe_crash();

    if (RtlGetCmdlineOption(KiGetCommandLine(), "xenon.selftest", NULL, 0))
        KiRunSelfTests();

    ConPrint("\nPhase 1.2 complete: threads, scheduler, DPCs, waits, objects and handles are up.\n");
    ConPrint("The shell (C:/#) arrives in Phase 1.5.\n");
    if (g_test_mode && !RtlGetCmdlineOption(KiGetCommandLine(), "xenon.interactive", NULL, 0))
        HalDebugExit(XENON_EXIT_HALTED);

    /* Hand over to the debug shell; the Init thread's work is done. */
    KdStartShell();
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* ---- Phase 0: entry point ----------------------------------------------- */

NORETURN void KiSystemStartup(void)
{
    HalSerialInit();
    if (!LIMINE_BASE_REVISION_SUPPORTED) {
        HalSerialWrite("XenonOS: bootloader does not support Limine base revision 3\n", 61);
        cpu_halt_forever();
    }
    capture_loader_block();
    g_test_mode = RtlGetCmdlineOption(KiGetCommandLine(), "xenon.debugexit", NULL, 0);

    struct limine_framebuffer_response *fbr = g_fb_req.response;
    if (fbr && fbr->framebuffer_count)
        FbConInit(fbr->framebuffers[0]);

    KiInitGdt();
    KiInitIdt();
    HalPicInitMasked();
    HalCalibrateTsc();

    print_banner();
    DbgPrint("Boot: %s %s on %s", KeLoaderBlock.BootloaderName, KeLoaderBlock.BootloaderVersion,
             KeLoaderBlock.FirmwareName);
    DbgPrint("Boot: kernel built %s, command line \"%s\"", XENON_BUILD_DATE, KiGetCommandLine());

    MmInitSystem();
    void *stack = MmCreateKernelStack(KERNEL_STACK_SIZE);
    g_boot_stack = stack;
    if (!stack)
        KeBugCheckEx(PHASE0_INITIALIZATION_FAILED, 1, 0, 0, 0);
    DbgPrint("Mm: kernel page tables active, NX %s, framebuffer write-combining",
             (HalReadMsr(0xC0000080) & (1u << 11)) ? "on" : "off");
    if (FbConPresent()) {
        unsigned w, h, fw, fh, cols, rows;
        FbConGetResolution(&w, &h, &fw, &fh);
        FbConGetSize(&cols, &rows);
        DbgPrint("Con: framebuffer %ux%u, font %ux%u, %ux%u text; serial COM1 %s", w, h, fw, fh, cols, rows,
                 HalSerialPresent() ? "present" : "not present");
    }
    KiSwitchStack(stack, KiSystemStartupPhase1);
}
