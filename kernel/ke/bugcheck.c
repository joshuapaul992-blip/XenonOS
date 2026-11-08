/*
 * Ke - bugcheck (the "stop screen").
 *
 * When the kernel hits a condition it cannot recover from, it stops the
 * machine and shows a Windows NT-style blue screen with a stop code, four
 * parameters, the registers and a stack trace. Everything is also written to
 * COM1 so the information survives even if the screen does not work.
 */
#include "ke.h"
#include "hal.h"
#include "rtl.h"
#include "console.h"
#include "../init/init.h"
#include <xenon/version.h>

static volatile int g_in_bugcheck;
extern volatile bool KiConsoleBypassLock;

static const char *stop_name(uint32_t code)
{
    switch (code) {
    case IRQL_NOT_LESS_OR_EQUAL:              return "IRQL_NOT_LESS_OR_EQUAL";
    case KMODE_EXCEPTION_NOT_HANDLED:         return "KMODE_EXCEPTION_NOT_HANDLED";
    case PAGE_FAULT_IN_NONPAGED_AREA:         return "PAGE_FAULT_IN_NONPAGED_AREA";
    case PHASE0_INITIALIZATION_FAILED:        return "PHASE0_INITIALIZATION_FAILED";
    case UNEXPECTED_KERNEL_MODE_TRAP:         return "UNEXPECTED_KERNEL_MODE_TRAP";
    case SYSTEM_THREAD_EXCEPTION_NOT_HANDLED: return "SYSTEM_THREAD_EXCEPTION_NOT_HANDLED";
    case KERNEL_SECURITY_CHECK_FAILURE:       return "KERNEL_SECURITY_CHECK_FAILURE";
    case MANUALLY_INITIATED_CRASH:            return "MANUALLY_INITIATED_CRASH";
    default:                                  return "UNKNOWN_STOP_CODE";
    }
}

static const char *exception_name(uint64_t v)
{
    static const char *names[32] = {
        "#DE Divide Error", "#DB Debug", "NMI Interrupt", "#BP Breakpoint",
        "#OF Overflow", "#BR BOUND Range Exceeded", "#UD Invalid Opcode", "#NM Device Not Available",
        "#DF Double Fault", "Coprocessor Segment Overrun", "#TS Invalid TSS", "#NP Segment Not Present",
        "#SS Stack-Segment Fault", "#GP General Protection", "#PF Page Fault", "Reserved (15)",
        "#MF x87 Floating-Point Error", "#AC Alignment Check", "#MC Machine Check", "#XM SIMD Floating-Point",
        "#VE Virtualization", "#CP Control Protection", "Reserved (22)", "Reserved (23)",
        "Reserved (24)", "Reserved (25)", "Reserved (26)", "Reserved (27)",
        "#HV Hypervisor Injection", "#VC VMM Communication", "#SX Security", "Reserved (31)",
    };
    return v < 32 ? names[v] : "Interrupt";
}

static bool kernel_address(uint64_t a)
{
    return a >= 0xFFFF800000000000ull;
}

static void print_stack_trace(uint64_t rip, uint64_t rbp)
{
    ConPrint("Stack trace (use tools/symbolize.sh to get function names):\n");
    ConPrint("  0x%016llx\n", (unsigned long long)rip);
    for (int depth = 0; depth < 12; depth++) {
        if (!kernel_address(rbp) || (rbp & 7))
            break;
        uint64_t *frame = (uint64_t *)rbp;
        uint64_t ret = frame[1];
        if (!kernel_address(ret))
            break;
        ConPrint("  0x%016llx\n", (unsigned long long)ret);
        if (frame[0] <= rbp)        /* frames must move up the stack */
            break;
        rbp = frame[0];
    }
}

NORETURN void KeBugCheckWithFrame(uint32_t code, uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4,
                                  const KTRAP_FRAME *f)
{
    cpu_cli();
    __asm__ volatile("mov %0, %%cr8" :: "r"((uint64_t)HIGH_LEVEL));
    KiConsoleBypassLock = true;
    if (__atomic_exchange_n(&g_in_bugcheck, 1, __ATOMIC_SEQ_CST)) {
        /* Crashed while showing the crash screen: just stop. */
        HalSerialWrite("\n*** nested bugcheck, halting\n", 30);
        if (KiTestMode())
            HalDebugExit(XENON_EXIT_BUGCHECK);
        cpu_halt_forever();
    }

    FbConSetColors(CON_WHITE, CON_BLUE);
    FbConClear();
    FbConWrite("\x1b[?25l", 6);            /* hide the cursor */

    ConPrint("\n*** STOP: 0x%08X (0x%016llX,0x%016llX,0x%016llX,0x%016llX)\n",
             code, (unsigned long long)p1, (unsigned long long)p2,
             (unsigned long long)p3, (unsigned long long)p4);
    ConPrint("%s\n\n", stop_name(code));
    ConPrint("%s has stopped to prevent damage to your computer.\n\n", XENON_VERSION_STR);
    KTHREAD *thread = KeGetCurrentThread();
    if (thread)
        ConPrint("Thread: %s (TID %llu)\n", thread->Name, (unsigned long long)thread->Cid);


    if (f) {
        ConPrint("Exception: %s (vector %llu, error code 0x%llx)\n",
                 exception_name(f->Vector), (unsigned long long)f->Vector, (unsigned long long)f->ErrorCode);
        if (f->Vector == 14)
            ConPrint("Fault address: 0x%016llx (%s, %s, %s)\n", (unsigned long long)read_cr2(),
                     (f->ErrorCode & 1) ? "protection violation" : "page not present",
                     (f->ErrorCode & 2) ? "write" : ((f->ErrorCode & 16) ? "instruction fetch" : "read"),
                     (f->ErrorCode & 4) ? "user mode" : "kernel mode");
        ConPrint("\n");
        ConPrint("RAX=%016llx RBX=%016llx RCX=%016llx RDX=%016llx\n",
                 (unsigned long long)f->Rax, (unsigned long long)f->Rbx,
                 (unsigned long long)f->Rcx, (unsigned long long)f->Rdx);
        ConPrint("RSI=%016llx RDI=%016llx RBP=%016llx RSP=%016llx\n",
                 (unsigned long long)f->Rsi, (unsigned long long)f->Rdi,
                 (unsigned long long)f->Rbp, (unsigned long long)f->Rsp);
        ConPrint("R8 =%016llx R9 =%016llx R10=%016llx R11=%016llx\n",
                 (unsigned long long)f->R8, (unsigned long long)f->R9,
                 (unsigned long long)f->R10, (unsigned long long)f->R11);
        ConPrint("R12=%016llx R13=%016llx R14=%016llx R15=%016llx\n",
                 (unsigned long long)f->R12, (unsigned long long)f->R13,
                 (unsigned long long)f->R14, (unsigned long long)f->R15);
        ConPrint("RIP=%016llx RFL=%08llx CS=%04llx SS=%04llx\n",
                 (unsigned long long)f->Rip, (unsigned long long)f->Rflags,
                 (unsigned long long)f->Cs, (unsigned long long)f->Ss);
        ConPrint("CR0=%016llx CR2=%016llx CR3=%016llx CR4=%016llx\n\n",
                 (unsigned long long)read_cr0(), (unsigned long long)read_cr2(),
                 (unsigned long long)read_cr3(), (unsigned long long)read_cr4());
        print_stack_trace(f->Rip, f->Rbp);
    } else {
        uint64_t rbp;
        __asm__ volatile("mov %%rbp, %0" : "=r"(rbp));
        print_stack_trace((uint64_t)__builtin_return_address(0), rbp);
    }

    ConPrint("\nRestart your computer. If this screen appears again, check for new\n"
             "hardware or drivers. A copy of this information was sent to COM1.\n");

    if (KiTestMode())
        HalDebugExit(XENON_EXIT_BUGCHECK);
    cpu_halt_forever();
}

NORETURN void KeBugCheckEx(uint32_t code, uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4)
{
    KeBugCheckWithFrame(code, p1, p2, p3, p4, NULL);
}

NORETURN void KeAssertFailed(const char *expr, const char *file, int line)
{
    ConPrint("\nAssertion failed: %s (%s:%d)\n", expr, file, line);
    KeBugCheckEx(KERNEL_SECURITY_CHECK_FAILURE, (uint64_t)expr, (uint64_t)file, (uint64_t)line, 0);
}
