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

#define BAD_POOL_HEADER                 0x00000019
#define PFN_LIST_CORRUPT                0x0000004E
#define NO_PAGES_AVAILABLE              0x0000004D
#define BAD_POOL_CALLER                 0x000000C2
#define HAL_INITIALIZATION_FAILED       0x0000005C
#define ACPI_BIOS_ERROR                 0x000000A5

/* ---- Spin locks ---------------------------------------------------------
 * Phase 1.1: one CPU, so a spin lock just disables interrupts and the lock
 * word catches recursion. Phase 1.2 adds IRQL; SMP comes later. */

typedef struct { volatile uint32_t Locked; } KSPIN_LOCK;
typedef uint64_t KIRQL_STATE;

static inline KIRQL_STATE KeAcquireSpinLock(KSPIN_LOCK *lock)
{
    uint64_t flags = read_rflags();
    cpu_cli();
    while (__atomic_exchange_n(&lock->Locked, 1, __ATOMIC_ACQUIRE))
        cpu_pause();
    return flags;
}

static inline void KeReleaseSpinLock(KSPIN_LOCK *lock, KIRQL_STATE state)
{
    __atomic_store_n(&lock->Locked, 0, __ATOMIC_RELEASE);
    if (state & (1u << 9))
        cpu_sti();
}

/* ---- Interrupts --------------------------------------------------------- */

/* Vector layout:
 *   0x00-0x1F  CPU exceptions
 *   0x20-0x2F  legacy PIC (remapped, masked)
 *   0x30-0xEF  device interrupts (allocated by KeAllocateVector)
 *   0xF0       local APIC timer (system clock tick)
 *   0xFE       APIC error
 *   0xFF       APIC spurious
 */
#define VECTOR_DEVICE_FIRST   0x30
#define VECTOR_DEVICE_LAST    0xEF
#define VECTOR_CLOCK          0xF0
#define VECTOR_APIC_ERROR     0xFE
#define VECTOR_SPURIOUS       0xFF

typedef void (*KINTERRUPT_ROUTINE)(KTRAP_FRAME *frame, void *context);

/* Returns a free device vector, or 0 if none are left. */
uint8_t KeAllocateVector(void);
void KeConnectInterrupt(uint8_t vector, KINTERRUPT_ROUTINE routine, void *context);
void KeDisconnectInterrupt(uint8_t vector);
uint64_t KeQueryInterruptCount(uint8_t vector);

/* ---- System clock ------------------------------------------------------- */

#define KE_CLOCK_HZ 1000

extern volatile uint64_t KeTickCount;   /* clock ticks since the tick started */
uint64_t KeQueryUptimeNs(void);          /* nanoseconds since boot */
