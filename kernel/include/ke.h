/* SPDX-License-Identifier: MIT */
/*
 * Ke - kernel core: stop screen, IRQL, spin locks, interrupts, DPCs,
 * dispatcher objects (events, mutexes, semaphores, timers, threads), waits
 * and the scheduler.
 */
#pragma once
#include "xenon.h"
#include "list.h"
#include "status.h"

/* ---- Stop codes (same numbers as Windows NT where one exists) ------------ */

#define IRQL_NOT_LESS_OR_EQUAL              0x0000000A
#define BAD_POOL_HEADER                     0x00000019
#define KMODE_EXCEPTION_NOT_HANDLED         0x0000001E
#define PHASE0_INITIALIZATION_FAILED        0x00000031
#define PHASE1_INITIALIZATION_FAILED        0x00000032
#define NO_PAGES_AVAILABLE                  0x0000004D
#define PFN_LIST_CORRUPT                    0x0000004E
#define PAGE_FAULT_IN_NONPAGED_AREA         0x00000050
#define HAL_INITIALIZATION_FAILED           0x0000005C
#define SYSTEM_THREAD_EXCEPTION_NOT_HANDLED 0x0000007E
#define UNEXPECTED_KERNEL_MODE_TRAP         0x0000007F
#define ACPI_BIOS_ERROR                     0x000000A5
#define BAD_POOL_CALLER                     0x000000C2
#define MANUALLY_INITIATED_CRASH            0x000000E2
#define KERNEL_SECURITY_CHECK_FAILURE       0x00000139
#define INVALID_DATA_ACCESS_TRAP            0x00000004
#define IRQL_GT_ZERO_AT_SYSTEM_SERVICE      0x0000004A
#define THREAD_NOT_MUTEX_OWNER              0x00000011
#define MUTEX_LEVEL_NUMBER_VIOLATION        0x0000000D
#define ATTEMPTED_SWITCH_FROM_DPC           0x000000B8
#define REFERENCE_BY_POINTER                0x00000018
#define OBJECT_INITIALIZATION_FAILED        0x0000005E

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

void KiDispatchInterrupt(KTRAP_FRAME *frame);
void KiInitGdt(void);
void KiInitIdt(void);
void KiSetKernelStack(uint64_t rsp0);

/* ---- IRQL ----------------------------------------------------------------
 * The interrupt request level is kept in CR8 (the local APIC task priority).
 * A hardware interrupt on vector V is only delivered while IRQL < V >> 4,
 * and its handler runs at IRQL V >> 4. */

typedef uint8_t KIRQL;

#define PASSIVE_LEVEL   0
#define APC_LEVEL       1
#define DISPATCH_LEVEL  2
#define CLOCK_LEVEL     13
#define IPI_LEVEL       14
#define HIGH_LEVEL      15

static inline KIRQL KeGetCurrentIrql(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr8, %0" : "=r"(v));
    return (KIRQL)v;
}

static inline KIRQL KeRaiseIrql(KIRQL irql)
{
    KIRQL old = KeGetCurrentIrql();
    if (irql < old)
        KeBugCheckEx(IRQL_NOT_LESS_OR_EQUAL, irql, old, 0, 1);
    __asm__ volatile("mov %0, %%cr8" :: "r"((uint64_t)irql) : "memory");
    return old;
}

/* Lowering below DISPATCH_LEVEL runs pending DPCs and thread switches. */
void KeLowerIrql(KIRQL irql);
static inline KIRQL KeRaiseIrqlToDpcLevel(void) { return KeRaiseIrql(DISPATCH_LEVEL); }

/* ---- Spin locks ------------------------------------------------------------
 * KeAcquireSpinLock raises to DISPATCH_LEVEL (no thread switches; interrupts
 * still arrive). KeAcquireSpinLockRaiseToHigh also masks every interrupt and
 * is for data shared with interrupt handlers. Both are released with
 * KeReleaseSpinLock. */

typedef struct { volatile uint32_t Locked; } KSPIN_LOCK;
typedef KIRQL KIRQL_STATE;

static inline KIRQL KeAcquireSpinLockAtLevel(KSPIN_LOCK *lock, KIRQL level)
{
    KIRQL old = KeGetCurrentIrql();
    if (old < level)
        __asm__ volatile("mov %0, %%cr8" :: "r"((uint64_t)level) : "memory");
    while (__atomic_exchange_n(&lock->Locked, 1, __ATOMIC_ACQUIRE))
        cpu_pause();
    return old;
}

static inline KIRQL KeAcquireSpinLock(KSPIN_LOCK *lock) { return KeAcquireSpinLockAtLevel(lock, DISPATCH_LEVEL); }
static inline KIRQL KeAcquireSpinLockRaiseToHigh(KSPIN_LOCK *lock) { return KeAcquireSpinLockAtLevel(lock, HIGH_LEVEL); }

static inline void KeReleaseSpinLock(KSPIN_LOCK *lock, KIRQL old)
{
    __atomic_store_n(&lock->Locked, 0, __ATOMIC_RELEASE);
    if (old < KeGetCurrentIrql())
        KeLowerIrql(old);
}

/* For code already at DISPATCH_LEVEL or above. */
static inline void KeAcquireSpinLockAtDpcLevel(KSPIN_LOCK *lock)
{
    while (__atomic_exchange_n(&lock->Locked, 1, __ATOMIC_ACQUIRE))
        cpu_pause();
}
static inline void KeReleaseSpinLockFromDpcLevel(KSPIN_LOCK *lock)
{
    __atomic_store_n(&lock->Locked, 0, __ATOMIC_RELEASE);
}

/* ---- Interrupts -------------------------------------------------------------
 *   0x00-0x1F  CPU exceptions
 *   0x20-0x2F  legacy PIC (remapped, masked)
 *   0x30-0xCF  device interrupts, IRQL 3-12 (allocated by KeAllocateVector)
 *   0xD0       local APIC timer: the clock, CLOCK_LEVEL
 *   0xFE       APIC error, 0xFF APIC spurious */

#define VECTOR_DEVICE_FIRST   0x30
#define VECTOR_DEVICE_LAST    0xCF
#define VECTOR_CLOCK          0xD0
#define VECTOR_APIC_ERROR     0xFE
#define VECTOR_SPURIOUS       0xFF

typedef void (*KINTERRUPT_ROUTINE)(KTRAP_FRAME *frame, void *context);

/* Allocate a free device vector at (or near) the requested IRQL (3-12). */
uint8_t KeAllocateVector(void);
uint8_t KeAllocateVectorAtIrql(KIRQL irql);
void KeConnectInterrupt(uint8_t vector, KINTERRUPT_ROUTINE routine, void *context);
void KeDisconnectInterrupt(uint8_t vector);
uint64_t KeQueryInterruptCount(uint8_t vector);

/* ---- DPCs ------------------------------------------------------------------ */

struct KDPC;
typedef void (*KDEFERRED_ROUTINE)(struct KDPC *dpc, void *context, void *arg1, void *arg2);

typedef struct KDPC {
    LIST_ENTRY Entry;
    KDEFERRED_ROUTINE Routine;
    void *Context;
    void *Arg1, *Arg2;
    volatile bool Inserted;
} KDPC;

void KeInitializeDpc(KDPC *dpc, KDEFERRED_ROUTINE routine, void *context);
/* Safe at any IRQL. Returns false if the DPC was already queued. */
bool KeInsertQueueDpc(KDPC *dpc, void *arg1, void *arg2);
bool KeRemoveQueueDpc(KDPC *dpc);

/* ---- Dispatcher objects ---------------------------------------------------- */

typedef enum {
    NotificationEvent = 0,       /* stays signaled until reset */
    SynchronizationEvent = 1,    /* wakes one waiter, then resets itself */
    MutantObject = 2,
    SemaphoreObject = 5,
    ThreadObject = 6,
    NotificationTimer = 8,
    SynchronizationTimer = 9,
    ProcessObject = 3,
} KOBJECTS;

typedef struct DISPATCHER_HEADER {
    uint8_t  Type;
    int32_t  SignalState;
    LIST_ENTRY WaitListHead;
} DISPATCHER_HEADER;

typedef struct KEVENT { DISPATCHER_HEADER Header; } KEVENT;

struct KTHREAD;
typedef struct KMUTANT {
    DISPATCHER_HEADER Header;            /* SignalState 1 = free, <= 0 = owned (-recursion+1) */
    LIST_ENTRY MutantListEntry;          /* in the owner's list */
    struct KTHREAD *OwnerThread;
    bool Abandoned;
} KMUTANT, KMUTEX;

typedef struct KSEMAPHORE {
    DISPATCHER_HEADER Header;            /* SignalState = count */
    int32_t Limit;
} KSEMAPHORE;

typedef struct KTIMER {
    DISPATCHER_HEADER Header;
    LIST_ENTRY TimerListEntry;
    uint64_t DueTime;                    /* absolute, ns since boot */
    uint32_t PeriodMs;
    KDPC *Dpc;
    bool Inserted;
    struct KTHREAD *WaitThread;          /* set for a thread's own timeout timer */
} KTIMER;

void KeInitializeEvent(KEVENT *e, KOBJECTS type, bool state);
int32_t KeSetEvent(KEVENT *e);            /* returns the previous state */
int32_t KeResetEvent(KEVENT *e);
void KeClearEvent(KEVENT *e);
int32_t KePulseEvent(KEVENT *e);
int32_t KeReadStateEvent(KEVENT *e);

void KeInitializeMutex(KMUTEX *m);
int32_t KeReleaseMutex(KMUTEX *m);        /* bugchecks if the caller is not the owner */
NTSTATUS KeReleaseMutant(KMUTANT *m, bool abandoned);

void KeInitializeSemaphore(KSEMAPHORE *s, int32_t count, int32_t limit);
NTSTATUS KeReleaseSemaphore(KSEMAPHORE *s, int32_t adjustment, int32_t *previous);

void KeInitializeTimer(KTIMER *t);
void KeInitializeTimerEx(KTIMER *t, KOBJECTS type);
/* due_ns < 0: relative to now; > 0: absolute uptime in ns. Returns true if the
   timer was already set. */
bool KeSetTimer(KTIMER *t, int64_t due_ns, KDPC *dpc);
bool KeSetTimerEx(KTIMER *t, int64_t due_ns, uint32_t period_ms, KDPC *dpc);
bool KeCancelTimer(KTIMER *t);
bool KeReadStateTimer(KTIMER *t);

/* ---- Waits ----------------------------------------------------------------- */

#define MAXIMUM_WAIT_OBJECTS 64
#define THREAD_WAIT_OBJECTS  4
#define KE_INFINITE          ((int64_t)INT64_MIN)

typedef enum { WaitAll, WaitAny } WAIT_TYPE;

typedef struct KWAIT_BLOCK {
    LIST_ENTRY WaitListEntry;            /* in the object's wait list */
    struct KTHREAD *Thread;
    DISPATCHER_HEADER *Object;
    uint16_t WaitKey;                    /* index in the wait array */
    uint8_t  WaitType;
} KWAIT_BLOCK;

/* timeout_ns: KE_INFINITE to wait forever, 0 to poll, < 0 relative, > 0 absolute. */
NTSTATUS KeWaitForSingleObject(void *object, int64_t timeout_ns);
NTSTATUS KeWaitForMultipleObjects(unsigned count, void *objects[], WAIT_TYPE type, int64_t timeout_ns);
NTSTATUS KeDelayExecutionThread(int64_t interval_ns);   /* negative = relative */
static inline NTSTATUS KeSleepMs(unsigned ms) { return KeDelayExecutionThread(-(int64_t)ms * 1000000); }

/* ---- Threads ----------------------------------------------------------------- */

#define LOW_PRIORITY              0
#define LOW_REALTIME_PRIORITY     16
#define HIGH_PRIORITY             31
#define MAXIMUM_PRIORITY          32
#define THREAD_DEFAULT_PRIORITY   8
#define THREAD_QUANTUM_MS         20

typedef enum { Initialized, Ready, Running, Standby, Terminated, Waiting } KTHREAD_STATE;

typedef void (*KSTART_ROUTINE)(void *context);
struct KPROCESS;

typedef struct KTHREAD {
    DISPATCHER_HEADER Header;            /* signaled when the thread exits */
    uint64_t KernelRsp;                  /* saved stack pointer while switched out */
    void    *StackTop;
    size_t   StackSize;
    uint8_t  State;
    int8_t   Priority;
    int8_t   BasePriority;
    int16_t  Quantum;                    /* clock ticks left */
    bool     QuantumEnd;
    NTSTATUS WaitStatus;
    KWAIT_BLOCK WaitBlockFill[THREAD_WAIT_OBJECTS];   /* enough for most waits */
    KWAIT_BLOCK *WaitBlocks;             /* WaitBlockFill or a pool array for big waits */
    unsigned WaitCount;
    uint8_t  WaitType;
    KTIMER   Timer;                      /* timeouts */
    LIST_ENTRY ReadyListEntry;
    LIST_ENTRY ThreadListEntry;          /* all threads */
    LIST_ENTRY ProcessThreadEntry;       /* threads of its process */
    LIST_ENTRY MutantListHead;           /* mutexes it owns */
    KSTART_ROUTINE StartRoutine;
    void *StartContext;
    NTSTATUS ExitStatus;
    uint64_t KernelTime;                 /* clock ticks spent running */
    uint64_t ContextSwitches;
    uint64_t Cid;                        /* thread id */
    struct KPROCESS *Process;
    char Name[32];
} KTHREAD;

/* Set up a thread whose stack is already allocated. It starts when readied. */
void KeInitializeThread(KTHREAD *t, void *stack_top, size_t stack_size, KSTART_ROUTINE routine,
                        void *context, int priority, const char *name);
void KeReadyThread(KTHREAD *t);
KTHREAD *KeGetCurrentThread(void);
NORETURN void KeTerminateThread(NTSTATUS status);
int KeSetPriorityThread(KTHREAD *t, int priority);     /* returns the old priority */
void KeYieldExecution(void);

/* Iterate all threads (for tools). The callback runs at DISPATCH_LEVEL. */
void KeEnumerateThreads(void (*cb)(KTHREAD *t, void *ctx), void *ctx);
unsigned KeThreadCount(void);
const char *KeThreadStateName(uint8_t state);

/* Scheduler set-up: the calling context becomes the initial thread. */
void KiInitializeScheduler(KTHREAD *initial, KTHREAD *idle, void *idle_stack_top, size_t idle_stack_size);
/* Called when a terminated thread can be freed (stack and object). */
extern void (*KiThreadReaper)(KTHREAD *t);

/* Internal: dispatcher database (single CPU: DISPATCH_LEVEL serialises it). */
void KiReadyThread(KTHREAD *t);
void KiSwapThread(void);
void KiUnwaitThread(KTHREAD *t, NTSTATUS status);
void KiWaitTest(DISPATCHER_HEADER *object);
void KiInsertTimer(KTIMER *t, uint64_t due);
bool KiRemoveTimer(KTIMER *t);
void KiInitializeTimers(void);
void KiCheckTimers(void);       /* clock interrupt: queue the expiry DPC if due */

/* ---- System clock ------------------------------------------------------------ */

#define KE_CLOCK_HZ 1000

extern volatile uint64_t KeTickCount;     /* clock ticks since the tick started */
uint64_t KeQueryUptimeNs(void);           /* nanoseconds since boot */
void KiClockInterrupt(KTRAP_FRAME *frame, void *context);
uint64_t KeQueryContextSwitches(void);
uint64_t KeQueryIdleTicks(void);
