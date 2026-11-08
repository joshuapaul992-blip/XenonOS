/*
 * Ke - kernel core.
 */
#pragma once
#include "xenon.h"

/* Stop codes (same numbers as Windows NT where one exists). */
#define IRQL_NOT_LESS_OR_EQUAL          0x0000000A
#define KMODE_EXCEPTION_NOT_HANDLED     0x0000001E
#define PAGE_FAULT_IN_NONPAGED_AREA     0x00000050
#define PHASE0_INITIALIZATION_FAILED    0x00000031
#define UNEXPECTED_KERNEL_MODE_TRAP     0x0000007F
#define SYSTEM_THREAD_EXCEPTION_NOT_HANDLED 0x0000007E
#define KERNEL_SECURITY_CHECK_FAILURE   0x00000139
#define MANUALLY_INITIATED_CRASH        0x000000E2

/* Register state saved by the interrupt stubs (see isr.asm). */
typedef struct KTRAP_FRAME {
    uint64_t R15, R14, R13, R12, R11, R10, R9, R8;
    uint64_t Rbp, Rdi, Rsi, Rdx, Rcx, Rbx, Rax;
    uint64_t Vector, ErrorCode;
    uint64_t Rip, Cs, Rflags, Rsp, Ss;
} KTRAP_FRAME;

NORETURN void KeBugCheckEx(uint32_t code, uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4);
NORETURN void KeBugCheckWithFrame(uint32_t code, uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4,
                                  const KTRAP_FRAME *frame);
#define KeBugCheck(code) KeBugCheckEx((code), 0, 0, 0, 0)

#define ASSERT(cond) do { if (!(cond)) KeAssertFailed(#cond, __FILE__, __LINE__); } while (0)
NORETURN void KeAssertFailed(const char *expr, const char *file, int line);

/* Called from the assembly interrupt stubs. */
void KiDispatchInterrupt(KTRAP_FRAME *frame);

/* Architecture setup */
void KiInitGdt(void);
void KiInitIdt(void);
