/*
 * Ke - IRQL, DPCs, the scheduler and the clock tick.
 *
 * One processor control block (KPRCB) holds the scheduler state. Everything
 * in it is changed at DISPATCH_LEVEL, which on a single CPU is enough to keep
 * it consistent: thread switches and DPCs only happen when IRQL drops below
 * DISPATCH_LEVEL, and interrupt handlers only set the "pending" flags.
 *
 * Scheduling is priority-based (0-31) and preemptive: the highest-priority
 * ready thread runs; threads of equal priority share the CPU in time slices
 * of THREAD_QUANTUM_MS; a thread that becomes ready with a higher priority
 * than the running one takes over as soon as IRQL allows.
 */
#include "ke.h"
#include "hal.h"
#include "rtl.h"
#include "console.h"

void KiSwapContext(uint64_t *old_rsp, uint64_t new_rsp);
void KiThreadStartupStub(void);

typedef struct {
    KTHREAD *CurrentThread;
    KTHREAD *IdleThread;
    LIST_ENTRY DpcQueue;
    KSPIN_LOCK DpcLock;
    volatile bool DpcPending;
    volatile bool ReschedulePending;
    LIST_ENTRY ReadyQueue[MAXIMUM_PRIORITY];
    uint32_t ReadySummary;
    bool SchedulerReady;
    KTHREAD *ReapThread;
    LIST_ENTRY ThreadList;
    unsigned ThreadCount;
    uint64_t ContextSwitches;
    uint64_t IdleTicks;
} KPRCB;

static KPRCB g_prcb;
void (*KiThreadReaper)(KTHREAD *t);
static KSPIN_LOCK g_dispatcher_lock;

/* ---- DPCs ---------------------------------------------------------------- */

void KeInitializeDpc(KDPC *dpc, KDEFERRED_ROUTINE routine, void *context)
{
    dpc->Routine = routine;
    dpc->Context = context;
    dpc->Inserted = false;
    InitializeListHead(&dpc->Entry);
}

bool KeInsertQueueDpc(KDPC *dpc, void *arg1, void *arg2)
{
    KIRQL old = KeAcquireSpinLockAtLevel(&g_prcb.DpcLock, HIGH_LEVEL);
    bool queued = false;
    if (!dpc->Inserted) {
        dpc->Inserted = true;
        dpc->Arg1 = arg1;
        dpc->Arg2 = arg2;
        InsertTailList(&g_prcb.DpcQueue, &dpc->Entry);
        g_prcb.DpcPending = true;
        queued = true;
    }
    __atomic_store_n(&g_prcb.DpcLock.Locked, 0, __ATOMIC_RELEASE);
    __asm__ volatile("mov %0, %%cr8" :: "r"((uint64_t)old) : "memory");
    /* If we were at PASSIVE/APC level, deliver it now. */
    if (queued && old < DISPATCH_LEVEL && g_prcb.SchedulerReady) {
        KIRQL o = KeRaiseIrql(DISPATCH_LEVEL);
        KeLowerIrql(o);
    }
    return queued;
}

bool KeRemoveQueueDpc(KDPC *dpc)
{
    KIRQL old = KeAcquireSpinLockAtLevel(&g_prcb.DpcLock, HIGH_LEVEL);
    bool was = dpc->Inserted;
    if (was) {
        RemoveEntryList(&dpc->Entry);
        dpc->Inserted = false;
    }
    __atomic_store_n(&g_prcb.DpcLock.Locked, 0, __ATOMIC_RELEASE);
    __asm__ volatile("mov %0, %%cr8" :: "r"((uint64_t)old) : "memory");
    return was;
}

static void retire_dpc_list(void)
{
    for (;;) {
        KIRQL old = KeAcquireSpinLockAtLevel(&g_prcb.DpcLock, HIGH_LEVEL);
        if (IsListEmpty(&g_prcb.DpcQueue)) {
            g_prcb.DpcPending = false;
            __atomic_store_n(&g_prcb.DpcLock.Locked, 0, __ATOMIC_RELEASE);
            __asm__ volatile("mov %0, %%cr8" :: "r"((uint64_t)old) : "memory");
            return;
        }
        KDPC *dpc = CONTAINING_RECORD(RemoveHeadList(&g_prcb.DpcQueue), KDPC, Entry);
        dpc->Inserted = false;
        KDEFERRED_ROUTINE routine = dpc->Routine;
        void *ctx = dpc->Context, *a1 = dpc->Arg1, *a2 = dpc->Arg2;
        __atomic_store_n(&g_prcb.DpcLock.Locked, 0, __ATOMIC_RELEASE);
        __asm__ volatile("mov %0, %%cr8" :: "r"((uint64_t)old) : "memory");
        routine(dpc, ctx, a1, a2);
        if (KeGetCurrentIrql() != DISPATCH_LEVEL)
            KeBugCheckEx(IRQL_NOT_LESS_OR_EQUAL, KeGetCurrentIrql(), DISPATCH_LEVEL, (uint64_t)routine, 2);
    }
}

/* ---- Ready queues --------------------------------------------------------- */

static int highest_ready(void)
{
    return g_prcb.ReadySummary ? 31 - __builtin_clz(g_prcb.ReadySummary) : -1;
}

static void insert_ready(KTHREAD *t, bool at_head)
{
    t->State = Ready;
    if (at_head)
        InsertHeadList(&g_prcb.ReadyQueue[t->Priority], &t->ReadyListEntry);
    else
        InsertTailList(&g_prcb.ReadyQueue[t->Priority], &t->ReadyListEntry);
    g_prcb.ReadySummary |= 1u << t->Priority;
}

static void remove_ready(KTHREAD *t)
{
    if (RemoveEntryList(&t->ReadyListEntry))
        g_prcb.ReadySummary &= ~(1u << t->Priority);
}

static KTHREAD *pop_ready(void)
{
    int p = highest_ready();
    if (p < 0)
        return g_prcb.IdleThread;
    KTHREAD *t = CONTAINING_RECORD(g_prcb.ReadyQueue[p].Flink, KTHREAD, ReadyListEntry);
    remove_ready(t);
    return t;
}

void KiReadyThread(KTHREAD *t)
{
    if (t == g_prcb.IdleThread)
        return;
    insert_ready(t, false);
    if (g_prcb.CurrentThread && t->Priority > g_prcb.CurrentThread->Priority)
        g_prcb.ReschedulePending = true;
    if (g_prcb.CurrentThread == g_prcb.IdleThread)
        g_prcb.ReschedulePending = true;
}

void KeReadyThread(KTHREAD *t)
{
    KIRQL old = KeAcquireSpinLock(&g_dispatcher_lock);
    KiReadyThread(t);
    KeReleaseSpinLock(&g_dispatcher_lock, old);
}

/* ---- Context switching ---------------------------------------------------- */

static void post_switch(void)
{
    KTHREAD *dead = g_prcb.ReapThread;
    if (dead && dead != g_prcb.CurrentThread) {
        g_prcb.ReapThread = NULL;
        if (KiThreadReaper)
            KiThreadReaper(dead);
    }
}

static void switch_to(KTHREAD *next)
{
    KTHREAD *prev = g_prcb.CurrentThread;
    if (next == prev) {
        prev->State = Running;
        return;
    }
    if (prev == g_prcb.IdleThread)
        prev->State = Ready;                    /* idle is always runnable */
    g_prcb.CurrentThread = next;
    next->State = Running;
    next->ContextSwitches++;
    g_prcb.ContextSwitches++;
    if (next->Quantum <= 0)
        next->Quantum = THREAD_QUANTUM_MS * KE_CLOCK_HZ / 1000;
    KiSetKernelStack((uint64_t)next->StackTop);

    uint64_t flags = read_rflags();
    cpu_cli();
    KiSwapContext(&prev->KernelRsp, next->KernelRsp);
    /* Running as 'prev' again (or as a new thread in KiThreadStartup). */
    if (flags & (1u << 9))
        cpu_sti();
    post_switch();
}

/* The current thread has already been put in its new state (waiting,
   terminated, ready). Run the best thread instead. Called at DISPATCH_LEVEL. */
void KiSwapThread(void)
{
    if (KeGetCurrentIrql() != DISPATCH_LEVEL)
        KeBugCheckEx(IRQL_NOT_LESS_OR_EQUAL, KeGetCurrentIrql(), DISPATCH_LEVEL, 0, 3);
    switch_to(pop_ready());
}

/* Handle a reschedule request: preemption or end of a time slice. */
static void dispatch(void)
{
    g_prcb.ReschedulePending = false;
    KTHREAD *cur = g_prcb.CurrentThread;
    if (cur->State != Running)
        return;
    bool quantum_end = cur->QuantumEnd;
    cur->QuantumEnd = false;
    int best = highest_ready();
    if (best < 0)
        return;
    if (cur == g_prcb.IdleThread) {
        switch_to(pop_ready());
    } else if (best > cur->Priority) {
        insert_ready(cur, true);                /* preempted: keep its place */
        switch_to(pop_ready());
    } else if (quantum_end) {
        cur->Quantum = THREAD_QUANTUM_MS * KE_CLOCK_HZ / 1000;
        if (best >= cur->Priority) {
            insert_ready(cur, false);           /* time slice over: go to the back */
            switch_to(pop_ready());
        }
    }
}

/* Run pending DPCs and thread switches. Called at DISPATCH_LEVEL. */
static void deliver_pending(void)
{
    while (g_prcb.DpcPending || g_prcb.ReschedulePending) {
        if (g_prcb.DpcPending)
            retire_dpc_list();
        if (g_prcb.ReschedulePending)
            dispatch();
    }
}

void KeLowerIrql(KIRQL irql)
{
    KIRQL cur = KeGetCurrentIrql();
    if (irql > cur)
        KeBugCheckEx(IRQL_NOT_LESS_OR_EQUAL, irql, cur, 0, 4);
    if (irql < DISPATCH_LEVEL && g_prcb.SchedulerReady) {
        for (;;) {
            __asm__ volatile("mov %0, %%cr8" :: "r"((uint64_t)DISPATCH_LEVEL) : "memory");
            deliver_pending();
            /* Check once more with interrupts off so nothing slips in between
               the check and the final IRQL change. */
            uint64_t flags = read_rflags();
            cpu_cli();
            if (!g_prcb.DpcPending && !g_prcb.ReschedulePending) {
                __asm__ volatile("mov %0, %%cr8" :: "r"((uint64_t)irql) : "memory");
                if (flags & (1u << 9))
                    cpu_sti();
                return;
            }
            if (flags & (1u << 9))
                cpu_sti();
        }
    }
    __asm__ volatile("mov %0, %%cr8" :: "r"((uint64_t)irql) : "memory");
}

/* Interrupt exit path: interrupts are disabled on entry and on return. */
void KiLowerIrqlFromInterrupt(KIRQL irql)
{
    if (irql < DISPATCH_LEVEL && g_prcb.SchedulerReady) {
        while (g_prcb.DpcPending || g_prcb.ReschedulePending) {
            __asm__ volatile("mov %0, %%cr8" :: "r"((uint64_t)DISPATCH_LEVEL) : "memory");
            cpu_sti();
            deliver_pending();
            cpu_cli();
        }
    }
    __asm__ volatile("mov %0, %%cr8" :: "r"((uint64_t)irql) : "memory");
}

/* ---- Threads -------------------------------------------------------------- */

KTHREAD *KeGetCurrentThread(void)
{
    return g_prcb.CurrentThread;
}

void KiThreadStartup(void)
{
    post_switch();
    KTHREAD *t = g_prcb.CurrentThread;
    cpu_sti();
    KeLowerIrql(PASSIVE_LEVEL);
    t->StartRoutine(t->StartContext);
    KeTerminateThread(STATUS_SUCCESS);
}

void KeInitializeThread(KTHREAD *t, void *stack_top, size_t stack_size, KSTART_ROUTINE routine,
                        void *context, int priority, const char *name)
{
    memset(t, 0, offsetof(KTHREAD, Name));
    t->Header.Type = ThreadObject;
    t->Header.SignalState = 0;
    InitializeListHead(&t->Header.WaitListHead);
    InitializeListHead(&t->ReadyListEntry);
    InitializeListHead(&t->MutantListHead);
    InitializeListHead(&t->ProcessThreadEntry);
    KeInitializeTimer(&t->Timer);
    t->WaitBlocks = t->WaitBlockFill;
    t->StackTop = stack_top;
    t->StackSize = stack_size;
    t->State = Initialized;
    t->Priority = t->BasePriority = (int8_t)(priority < 0 ? 0 : priority > 31 ? 31 : priority);
    t->Quantum = THREAD_QUANTUM_MS * KE_CLOCK_HZ / 1000;
    t->StartRoutine = routine;
    t->StartContext = context;
    strlcpy(t->Name, name ? name : "", sizeof(t->Name));

    /* Initial stack as KiSwapContext expects it: six saved registers and a
       return address that leads into KiThreadStartup. */
    if (stack_top) {
        uint64_t *sp = (uint64_t *)stack_top;
        *--sp = 0;                               /* alignment / fake return */
        *--sp = (uint64_t)KiThreadStartupStub;
        for (int i = 0; i < 6; i++)
            *--sp = 0;
        t->KernelRsp = (uint64_t)sp;
    }

    KIRQL old = KeAcquireSpinLock(&g_dispatcher_lock);
    InsertTailList(&g_prcb.ThreadList, &t->ThreadListEntry);
    g_prcb.ThreadCount++;
    KeReleaseSpinLock(&g_dispatcher_lock, old);
}

NORETURN void KeTerminateThread(NTSTATUS status)
{
    KTHREAD *t = g_prcb.CurrentThread;
    if (t == g_prcb.IdleThread)
        KeBugCheckEx(SYSTEM_THREAD_EXCEPTION_NOT_HANDLED, (uint64_t)status, 0, 0, 1);
    KeRaiseIrql(DISPATCH_LEVEL);
    KeAcquireSpinLockAtDpcLevel(&g_dispatcher_lock);
    t->ExitStatus = status;

    /* Mutexes still held are released as "abandoned". */
    while (!IsListEmpty(&t->MutantListHead)) {
        KMUTANT *m = CONTAINING_RECORD(t->MutantListHead.Flink, KMUTANT, MutantListEntry);
        RemoveEntryList(&m->MutantListEntry);
        m->OwnerThread = NULL;
        m->Abandoned = true;
        m->Header.SignalState = 1;
        KiWaitTest(&m->Header);
    }
    KiRemoveTimer(&t->Timer);

    t->Header.SignalState = 1;
    KiWaitTest(&t->Header);
    RemoveEntryList(&t->ThreadListEntry);
    g_prcb.ThreadCount--;
    t->State = Terminated;
    g_prcb.ReapThread = t;
    KeReleaseSpinLockFromDpcLevel(&g_dispatcher_lock);
    KiSwapThread();
    KeBugCheckEx(SYSTEM_THREAD_EXCEPTION_NOT_HANDLED, (uint64_t)t, 0, 0, 2);   /* not reached */
}

int KeSetPriorityThread(KTHREAD *t, int priority)
{
    if (priority < 1) priority = 1;
    if (priority > 31) priority = 31;
    KIRQL old = KeAcquireSpinLock(&g_dispatcher_lock);
    int prev = t->Priority;
    if (t->State == Ready) {
        remove_ready(t);
        t->Priority = t->BasePriority = (int8_t)priority;
        insert_ready(t, false);
        if (priority > g_prcb.CurrentThread->Priority)
            g_prcb.ReschedulePending = true;
    } else {
        t->Priority = t->BasePriority = (int8_t)priority;
        if (t == g_prcb.CurrentThread && highest_ready() > priority)
            g_prcb.ReschedulePending = true;
    }
    KeReleaseSpinLock(&g_dispatcher_lock, old);
    return prev;
}

void KeYieldExecution(void)
{
    KIRQL old = KeAcquireSpinLock(&g_dispatcher_lock);
    KTHREAD *cur = g_prcb.CurrentThread;
    if (cur != g_prcb.IdleThread && highest_ready() >= cur->Priority) {
        insert_ready(cur, false);
        KeReleaseSpinLockFromDpcLevel(&g_dispatcher_lock);
        KiSwapThread();
        KeLowerIrql(old);
        return;
    }
    KeReleaseSpinLock(&g_dispatcher_lock, old);
}

void KeEnumerateThreads(void (*cb)(KTHREAD *t, void *ctx), void *ctx)
{
    KIRQL old = KeAcquireSpinLock(&g_dispatcher_lock);
    for (LIST_ENTRY *e = g_prcb.ThreadList.Flink; e != &g_prcb.ThreadList; e = e->Flink)
        cb(CONTAINING_RECORD(e, KTHREAD, ThreadListEntry), ctx);
    KeReleaseSpinLock(&g_dispatcher_lock, old);
}

unsigned KeThreadCount(void)
{
    return g_prcb.ThreadCount;
}

const char *KeThreadStateName(uint8_t s)
{
    static const char *names[] = { "Init", "Ready", "Running", "Standby", "Terminated", "Waiting" };
    return s < ARRAY_SIZE(names) ? names[s] : "?";
}

uint64_t KeQueryContextSwitches(void) { return g_prcb.ContextSwitches; }
uint64_t KeQueryIdleTicks(void) { return g_prcb.IdleTicks; }

static void idle_loop(void *ctx)
{
    UNUSED(ctx);
    for (;;) {
        cpu_sti();
        cpu_hlt();          /* the next interrupt may make a thread ready */
    }
}

void KiInitializeScheduler(KTHREAD *initial, KTHREAD *idle, void *idle_stack_top, size_t idle_stack_size)
{
    InitializeListHead(&g_prcb.DpcQueue);
    InitializeListHead(&g_prcb.ThreadList);
    for (int i = 0; i < MAXIMUM_PRIORITY; i++)
        InitializeListHead(&g_prcb.ReadyQueue[i]);
    KiInitializeTimers();

    /* The code running now becomes the initial thread; its stack is the
       kernel stack set up in phase 0. */
    KeInitializeThread(initial, NULL, 0, NULL, NULL, THREAD_DEFAULT_PRIORITY, "Init");
    initial->State = Running;
    g_prcb.CurrentThread = initial;

    KeInitializeThread(idle, idle_stack_top, idle_stack_size, idle_loop, NULL, LOW_PRIORITY, "Idle");
    g_prcb.IdleThread = idle;
    idle->State = Ready;            /* never in a ready queue: chosen when nothing else is */

    g_prcb.SchedulerReady = true;
}

/* ---- Clock ------------------------------------------------------------------ */

uint64_t KeQueryUptimeNs(void)
{
    return HalGetTimeNs();
}

void KiClockInterrupt(KTRAP_FRAME *frame, void *context)
{
    UNUSED(frame);
    UNUSED(context);
    __atomic_add_fetch(&KeTickCount, 1, __ATOMIC_RELAXED);
    if (!g_prcb.SchedulerReady)
        return;
    KTHREAD *cur = g_prcb.CurrentThread;
    cur->KernelTime++;
    if (cur == g_prcb.IdleThread) {
        g_prcb.IdleTicks++;
    } else if (--cur->Quantum <= 0) {
        cur->QuantumEnd = true;
        g_prcb.ReschedulePending = true;
    }
    KiCheckTimers();
}
