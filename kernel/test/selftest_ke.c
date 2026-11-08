/* SPDX-License-Identifier: MIT */
/*
 * Self-tests for Phase 1.2: IRQL and DPCs, dispatcher objects, waits,
 * timers, the scheduler, and the object manager.
 */
#include "xenon.h"
#include "ke.h"
#include "ob.h"
#include "ps.h"
#include "mm.h"
#include "rtl.h"
#include "console.h"

extern char KiSelfTestWhy[160];
#define CHECK(cond, ...) do { if (!(cond)) { snprintf(KiSelfTestWhy, sizeof(KiSelfTestWhy), __VA_ARGS__); return false; } } while (0)

static uint64_t now_ms(void) { return KeQueryUptimeNs() / 1000000; }

/* Start a system thread and keep a referenced pointer to it. */
static ETHREAD *start(KSTART_ROUTINE fn, void *ctx, int prio, const char *name)
{
    ETHREAD *t = NULL;
    if (!NT_SUCCESS(PsCreateSystemThread(NULL, fn, ctx, prio, name, &t)))
        return NULL;
    return t;
}

static bool join(ETHREAD *t, unsigned timeout_ms)
{
    NTSTATUS st = KeWaitForSingleObject(t, -(int64_t)timeout_ms * 1000000);
    ObDereferenceObject(t);
    return st == STATUS_WAIT_0;
}

/* ---- IRQL and DPCs ----------------------------------------------------- */

static volatile int g_dpc_runs;
static volatile KIRQL g_dpc_irql;
static void test_dpc(KDPC *d, void *ctx, void *a1, void *a2)
{
    UNUSED(d); UNUSED(ctx); UNUSED(a2);
    g_dpc_runs += (int)(uintptr_t)a1;
    g_dpc_irql = KeGetCurrentIrql();
}

bool KiTestIrqlDpc(void)
{
    CHECK(KeGetCurrentIrql() == PASSIVE_LEVEL, "thread runs at IRQL %u, not PASSIVE_LEVEL", KeGetCurrentIrql());
    KDPC dpc;
    KeInitializeDpc(&dpc, test_dpc, NULL);
    g_dpc_runs = 0;
    KeInsertQueueDpc(&dpc, (void *)1, NULL);
    CHECK(g_dpc_runs == 1, "DPC queued at PASSIVE_LEVEL did not run at once");
    CHECK(g_dpc_irql == DISPATCH_LEVEL, "DPC ran at IRQL %u", g_dpc_irql);

    KIRQL old = KeRaiseIrql(DISPATCH_LEVEL);
    KeInsertQueueDpc(&dpc, (void *)1, NULL);
    CHECK(!KeInsertQueueDpc(&dpc, (void *)1, NULL), "a DPC was queued twice");
    bool early = g_dpc_runs != 1;
    KeLowerIrql(old);
    CHECK(!early, "DPC ran while IRQL was DISPATCH_LEVEL");
    CHECK(g_dpc_runs == 2, "DPC did not run when IRQL dropped (runs=%d)", g_dpc_runs);

    old = KeRaiseIrql(DISPATCH_LEVEL);
    KeInsertQueueDpc(&dpc, (void *)1, NULL);
    CHECK(KeRemoveQueueDpc(&dpc), "KeRemoveQueueDpc failed");
    KeLowerIrql(old);
    CHECK(g_dpc_runs == 2, "removed DPC still ran");
    CHECK(KeGetCurrentIrql() == PASSIVE_LEVEL, "IRQL not restored");
    return true;
}

/* ---- Events ------------------------------------------------------------ */

static KEVENT g_ev_go, g_ev_done;
static volatile int g_ev_woken;
static void ev_waiter(void *ctx)
{
    UNUSED(ctx);
    KeWaitForSingleObject(&g_ev_go, KE_INFINITE);
    __atomic_add_fetch(&g_ev_woken, 1, __ATOMIC_RELAXED);
    KeSetEvent(&g_ev_done);
}

bool KiTestEvents(void)
{
    /* A notification event wakes every waiter. */
    KeInitializeEvent(&g_ev_go, NotificationEvent, false);
    KeInitializeEvent(&g_ev_done, SynchronizationEvent, false);
    g_ev_woken = 0;
    ETHREAD *t[4];
    for (int i = 0; i < 4; i++)
        t[i] = start(ev_waiter, NULL, 8, "EvWaiter");
    KeSleepMs(10);
    CHECK(g_ev_woken == 0, "waiters ran before the event was set");
    KeSetEvent(&g_ev_go);
    for (int i = 0; i < 4; i++)
        CHECK(join(t[i], 1000), "waiter %d never finished", i);
    CHECK(g_ev_woken == 4, "notification event woke %d of 4 waiters", g_ev_woken);
    CHECK(KeReadStateEvent(&g_ev_go) == 1, "notification event reset itself");

    /* A synchronization event wakes one waiter per KeSetEvent. */
    KeInitializeEvent(&g_ev_go, SynchronizationEvent, false);
    g_ev_woken = 0;
    for (int i = 0; i < 3; i++)
        t[i] = start(ev_waiter, NULL, 8, "EvWaiter");
    KeSleepMs(10);
    KeSetEvent(&g_ev_go);
    KeSleepMs(20);
    CHECK(g_ev_woken == 1, "synchronization event woke %d waiters, expected 1", g_ev_woken);
    CHECK(KeReadStateEvent(&g_ev_go) == 0, "synchronization event stayed signaled");
    KeSetEvent(&g_ev_go);
    KeSetEvent(&g_ev_go);   /* may be consumed or left signaled; both are fine */
    for (int i = 0; i < 3; i++) {
        KeSetEvent(&g_ev_go);
        CHECK(join(t[i], 1000), "sync waiter %d never finished", i);
    }
    CHECK(g_ev_woken == 3, "woken %d", g_ev_woken);

    /* Polling with timeout 0. */
    KeInitializeEvent(&g_ev_go, NotificationEvent, false);
    CHECK(KeWaitForSingleObject(&g_ev_go, 0) == STATUS_TIMEOUT, "poll of an unsignaled event did not time out");
    KeSetEvent(&g_ev_go);
    CHECK(KeWaitForSingleObject(&g_ev_go, 0) == STATUS_WAIT_0, "poll of a signaled event failed");
    return true;
}

/* ---- Mutexes ----------------------------------------------------------- */

static KMUTEX g_mx;
static volatile int g_mx_inside, g_mx_overlap, g_mx_count;
static void mx_worker(void *ctx)
{
    int n = (int)(uintptr_t)ctx;
    for (int i = 0; i < n; i++) {
        KeWaitForSingleObject(&g_mx, KE_INFINITE);
        if (__atomic_add_fetch(&g_mx_inside, 1, __ATOMIC_RELAXED) != 1)
            g_mx_overlap = 1;
        g_mx_count++;
        KeYieldExecution();           /* give others a chance to break in */
        __atomic_sub_fetch(&g_mx_inside, 1, __ATOMIC_RELAXED);
        KeReleaseMutex(&g_mx);
    }
}

static void mx_abandon(void *ctx)
{
    UNUSED(ctx);
    KeWaitForSingleObject(&g_mx, KE_INFINITE);
    /* exits while owning the mutex */
}

bool KiTestMutex(void)
{
    KeInitializeMutex(&g_mx);
    /* Recursive acquisition by the owner. */
    CHECK(KeWaitForSingleObject(&g_mx, 0) == STATUS_WAIT_0, "first acquire failed");
    CHECK(KeWaitForSingleObject(&g_mx, 0) == STATUS_WAIT_0, "recursive acquire failed");
    KeReleaseMutex(&g_mx);
    CHECK(g_mx.OwnerThread == KeGetCurrentThread(), "released too early");
    KeReleaseMutex(&g_mx);
    CHECK(g_mx.OwnerThread == NULL && g_mx.Header.SignalState == 1, "mutex not free after two releases");
    CHECK(KeReleaseMutant(&g_mx, false) == STATUS_MUTANT_NOT_OWNED, "release by non-owner accepted");

    /* Mutual exclusion between 8 threads. */
    g_mx_inside = g_mx_overlap = g_mx_count = 0;
    ETHREAD *t[8];
    for (int i = 0; i < 8; i++)
        t[i] = start(mx_worker, (void *)(uintptr_t)50, 8, "MxWorker");
    for (int i = 0; i < 8; i++)
        CHECK(join(t[i], 5000), "worker %d did not finish", i);
    CHECK(!g_mx_overlap, "two threads were inside the mutex at once");
    CHECK(g_mx_count == 400, "count %d, expected 400", g_mx_count);

    /* A mutex whose owner exits is "abandoned". */
    ETHREAD *a = start(mx_abandon, NULL, 8, "MxAbandon");
    CHECK(join(a, 1000), "abandoning thread did not exit");
    NTSTATUS st = KeWaitForSingleObject(&g_mx, -(int64_t)1000 * 1000000);
    CHECK(st == STATUS_ABANDONED_WAIT_0, "wait on abandoned mutex returned 0x%x", (unsigned)st);
    KeReleaseMutex(&g_mx);
    CHECK(KeWaitForSingleObject(&g_mx, 0) == STATUS_WAIT_0, "mutex unusable after abandonment");
    KeReleaseMutex(&g_mx);
    return true;
}

/* ---- Semaphores -------------------------------------------------------- */

static KSEMAPHORE g_sem;
static void sem_producer(void *ctx)
{
    UNUSED(ctx);
    for (int i = 0; i < 100; i++) {
        while (KeReleaseSemaphore(&g_sem, 1, NULL) == STATUS_SEMAPHORE_LIMIT_EXCEEDED)
            KeSleepMs(1);
        if (i % 10 == 0)
            KeSleepMs(1);
    }
}

bool KiTestSemaphore(void)
{
    KeInitializeSemaphore(&g_sem, 0, 5);
    ETHREAD *p = start(sem_producer, NULL, 8, "SemProducer");
    for (int i = 0; i < 100; i++)
        CHECK(KeWaitForSingleObject(&g_sem, -(int64_t)2000 * 1000000) == STATUS_WAIT_0, "consumer timed out at %d", i);
    CHECK(join(p, 1000), "producer did not finish");
    CHECK(g_sem.Header.SignalState == 0, "count %d left", g_sem.Header.SignalState);
    CHECK(KeReleaseSemaphore(&g_sem, 5, NULL) == STATUS_SUCCESS, "release to the limit failed");
    CHECK(KeReleaseSemaphore(&g_sem, 1, NULL) == STATUS_SEMAPHORE_LIMIT_EXCEEDED, "limit not enforced");
    return true;
}

/* ---- Timers ------------------------------------------------------------ */

static volatile int g_timer_dpcs;
static void timer_dpc(KDPC *d, void *ctx, void *a1, void *a2)
{
    UNUSED(d); UNUSED(ctx); UNUSED(a1); UNUSED(a2);
    g_timer_dpcs++;
}

bool KiTestTimers(void)
{
    KTIMER t;
    KeInitializeTimer(&t);
    uint64_t t0 = now_ms();
    KeSetTimer(&t, -50 * 1000000ll, NULL);
    CHECK(KeWaitForSingleObject(&t, -(int64_t)2000 * 1000000) == STATUS_WAIT_0, "one-shot timer never fired");
    uint64_t el = now_ms() - t0;
    CHECK(el >= 50 && el <= 150, "50 ms timer fired after %llu ms", (unsigned long long)el);
    CHECK(KeReadStateTimer(&t), "notification timer not signaled");

    /* Periodic timer with a DPC: 10 ms period for ~105 ms. */
    KDPC dpc;
    KeInitializeDpc(&dpc, timer_dpc, NULL);
    g_timer_dpcs = 0;
    KeSetTimerEx(&t, -10 * 1000000ll, 10, &dpc);
    KeSleepMs(105);
    KeCancelTimer(&t);
    int n = g_timer_dpcs;
    CHECK(n >= 7 && n <= 12, "periodic 10 ms timer fired %d times in 105 ms", n);
    KeSleepMs(30);
    CHECK(g_timer_dpcs == n, "timer kept firing after KeCancelTimer");

    /* Sleep accuracy. */
    t0 = KeQueryUptimeNs();
    KeSleepMs(20);
    uint64_t us = (KeQueryUptimeNs() - t0) / 1000;
    CHECK(us >= 20000 && us < 80000, "20 ms sleep took %llu us", (unsigned long long)us);

    /* Cancelled timers must not fire. */
    KeInitializeTimer(&t);
    KeSetTimer(&t, -20 * 1000000ll, NULL);
    CHECK(KeCancelTimer(&t), "cancel of a set timer reported it was not set");
    CHECK(KeWaitForSingleObject(&t, -(int64_t)50 * 1000000) == STATUS_TIMEOUT, "cancelled timer fired");
    return true;
}

/* ---- Wait any / all / timeout ------------------------------------------ */

static KEVENT g_wa, g_wb;
static void set_b_later(void *ctx)
{
    UNUSED(ctx);
    KeSleepMs(10);
    KeSetEvent(&g_wb);
}

bool KiTestWaits(void)
{
    KeInitializeEvent(&g_wa, NotificationEvent, false);
    KeInitializeEvent(&g_wb, NotificationEvent, false);
    void *objs[2] = { &g_wa, &g_wb };

    ETHREAD *t = start(set_b_later, NULL, 8, "SetLater");
    NTSTATUS st = KeWaitForMultipleObjects(2, objs, WaitAny, -(int64_t)1000 * 1000000);
    CHECK(st == STATUS_WAIT_0 + 1, "WaitAny returned 0x%x, expected 1", (unsigned)st);
    CHECK(join(t, 1000), "helper thread stuck");

    st = KeWaitForMultipleObjects(2, objs, WaitAll, -(int64_t)30 * 1000000);
    CHECK(st == STATUS_TIMEOUT, "WaitAll with one object unsignaled returned 0x%x", (unsigned)st);
    KeSetEvent(&g_wa);
    st = KeWaitForMultipleObjects(2, objs, WaitAll, -(int64_t)30 * 1000000);
    CHECK(st == STATUS_WAIT_0, "WaitAll with both signaled returned 0x%x", (unsigned)st);

    /* WaitAll must take both semaphore units at once, or neither. */
    KSEMAPHORE s1, s2;
    KeInitializeSemaphore(&s1, 1, 1);
    KeInitializeSemaphore(&s2, 0, 1);
    void *sems[2] = { &s1, &s2 };
    CHECK(KeWaitForMultipleObjects(2, sems, WaitAll, 0) == STATUS_TIMEOUT, "WaitAll succeeded without s2");
    CHECK(s1.Header.SignalState == 1, "WaitAll took s1 although it could not take s2");

    KEVENT never;
    KeInitializeEvent(&never, NotificationEvent, false);
    uint64_t t0 = now_ms();
    st = KeWaitForSingleObject(&never, -(int64_t)40 * 1000000);
    uint64_t el = now_ms() - t0;
    CHECK(st == STATUS_TIMEOUT, "timeout wait returned 0x%x", (unsigned)st);
    CHECK(el >= 40 && el <= 140, "40 ms timeout took %llu ms", (unsigned long long)el);
    return true;
}

/* ---- Scheduler --------------------------------------------------------- */

static volatile bool g_hi_ran;
static void high_prio(void *ctx)
{
    UNUSED(ctx);
    KeSleepMs(20);
    g_hi_ran = true;
}

bool KiTestPreemption(void)
{
    g_hi_ran = false;
    ETHREAD *t = start(high_prio, NULL, 12, "HighPriority");
    /* Spin without ever waiting: only preemption can let the other thread run. */
    uint64_t end = KeQueryUptimeNs() + 500ull * 1000000;
    while (!g_hi_ran && KeQueryUptimeNs() < end)
        cpu_pause();
    bool ran = g_hi_ran;
    CHECK(join(t, 1000), "high priority thread did not exit");
    CHECK(ran, "a higher-priority thread did not preempt a spinning thread");
    return true;
}

static volatile bool g_rr_stop;
static volatile uint64_t g_rr_count[2];
static void rr_spinner(void *ctx)
{
    int i = (int)(uintptr_t)ctx;
    while (!g_rr_stop)
        g_rr_count[i]++;
}

bool KiTestRoundRobin(void)
{
    g_rr_stop = false;
    g_rr_count[0] = g_rr_count[1] = 0;
    int oldp = KeSetPriorityThread(KeGetCurrentThread(), 12);   /* stay in control */
    ETHREAD *a = start(rr_spinner, (void *)0, 6, "Spinner0");
    ETHREAD *b = start(rr_spinner, (void *)1, 6, "Spinner1");
    KeSleepMs(300);
    g_rr_stop = true;
    bool ja = join(a, 1000), jb = join(b, 1000);
    KeSetPriorityThread(KeGetCurrentThread(), oldp);
    CHECK(ja && jb, "spinners did not stop");
    uint64_t x = g_rr_count[0], y = g_rr_count[1];
    CHECK(x && y, "one equal-priority thread never ran (%llu / %llu)", (unsigned long long)x, (unsigned long long)y);
    CHECK(x * 4 >= y && y * 4 >= x, "time slices unfair: %llu vs %llu", (unsigned long long)x, (unsigned long long)y);
    return true;
}

/* ---- 100 threads ------------------------------------------------------- */

static KMUTEX g_t100_mx;
static volatile int g_t100_count, g_t100_inside, g_t100_overlap;
static KEVENT g_t100_tick;
static volatile int g_t100_ticks;

static void t100_worker(void *ctx)
{
    uint32_t seed = (uint32_t)(uintptr_t)ctx * 2654435761u + 1;
    for (int i = 0; i < 20; i++) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        KeSleepMs(1 + seed % 8);
        KeWaitForSingleObject(&g_t100_mx, KE_INFINITE);
        if (__atomic_add_fetch(&g_t100_inside, 1, __ATOMIC_RELAXED) != 1)
            g_t100_overlap = 1;
        g_t100_count++;
        if (seed & 1)
            KeYieldExecution();
        __atomic_sub_fetch(&g_t100_inside, 1, __ATOMIC_RELAXED);
        KeReleaseMutex(&g_t100_mx);
        if ((seed & 7) == 0) {
            /* Sometimes also wait (briefly) on a shared synchronization event. */
            if (KeWaitForSingleObject(&g_t100_tick, -2 * 1000000ll) == STATUS_WAIT_0)
                __atomic_add_fetch(&g_t100_ticks, 1, __ATOMIC_RELAXED);
        }
    }
}

/* One round: 100 threads, all joined, all handles closed. */
static bool t100_round(uint64_t *elapsed_ms)
{
    HANDLE h[100];
    unsigned threads0 = KeThreadCount();
    g_t100_count = g_t100_inside = g_t100_overlap = 0;
    for (int i = 0; i < 100; i++)
        CHECK(NT_SUCCESS(PsCreateSystemThread(&h[i], t100_worker, (void *)(uintptr_t)(i + 1), 7 + i % 3,
                                              "Worker", NULL)), "thread %d could not be created", i);
    CHECK(KeThreadCount() >= threads0 + 90, "only %u threads exist", KeThreadCount());

    uint64_t t0 = now_ms();
    for (int i = 0; i < 100 && KeThreadCount() > threads0; i++) {
        KeSetEvent(&g_t100_tick);          /* pulse the shared event while they run */
        KeSleepMs(2);
    }
    /* Wait for all of them: two WaitAll calls of 50 thread objects each. */
    for (int g = 0; g < 2; g++) {
        void *objs[50];
        for (int i = 0; i < 50; i++)
            CHECK(NT_SUCCESS(ObReferenceObjectByHandle(h[g * 50 + i], PsThreadType, &objs[i])), "bad handle");
        NTSTATUS st = KeWaitForMultipleObjects(50, objs, WaitAll, -(int64_t)20000 * 1000000);
        for (int i = 0; i < 50; i++)
            ObDereferenceObject(objs[i]);
        CHECK(st == STATUS_WAIT_0, "WaitAll on 50 threads returned 0x%x", (unsigned)st);
    }
    *elapsed_ms = now_ms() - t0;
    for (int i = 0; i < 100; i++)
        CHECK(ObCloseHandle(h[i]) == STATUS_SUCCESS, "closing handle %d failed", i);
    KeSleepMs(5);       /* let the last exited thread be reaped */
    CHECK(!g_t100_overlap, "mutex let two threads in at once");
    CHECK(g_t100_count == 2000, "%d mutex acquisitions, expected 2000", g_t100_count);
    CHECK(KeThreadCount() == threads0, "%u threads left, expected %u", KeThreadCount(), threads0);
    return true;
}

bool KiTestThreads100(void)
{
    KeInitializeMutex(&g_t100_mx);
    KeInitializeEvent(&g_t100_tick, SynchronizationEvent, false);
    g_t100_ticks = 0;
    uint64_t el;

    /* Round 1 warms up: the stack area and handle table reach their size. */
    if (!t100_round(&el))
        return false;

    MM_STATS m0, m1;
    POOL_STATS p0, p1;
    uint32_t tobjs0 = PsThreadType->TotalObjects;
    MmQueryStats(&m0);
    ExQueryPoolStats(&p0);

    /* Round 2 must give back everything it takes. */
    if (!t100_round(&el))
        return false;

    MmQueryStats(&m1);
    ExQueryPoolStats(&p1);
    CHECK(PsThreadType->TotalObjects == tobjs0, "%u thread objects left, expected %u",
          PsThreadType->TotalObjects, tobjs0);
    int64_t grown = (int64_t)p1.PagesInPool - (int64_t)p0.PagesInPool;
    CHECK((int64_t)m1.FreePages + grown == (int64_t)m0.FreePages, "page leak: %lld pages",
          (long long)((int64_t)m0.FreePages - grown - (int64_t)m1.FreePages));
    CHECK(p1.LiveAllocations == p0.LiveAllocations, "pool leak: %llu live allocations before, %llu after",
          (unsigned long long)p0.LiveAllocations, (unsigned long long)p1.LiveAllocations);
    DbgPrint("SELFTEST: threads100: 2 x 100 threads, 4000 mutex acquisitions, %d event waits, %llu ms per round",
             g_t100_ticks, (unsigned long long)el);
    return true;
}

/* ---- Object manager ---------------------------------------------------- */

static int g_deleted;
static void counting_delete(void *o) { UNUSED(o); g_deleted++; }

bool KiTestObjects(void)
{
    void *o;
    /* The namespace has the standard directories and types. */
    CHECK(NT_SUCCESS(ObReferenceObjectByName("\\ObjectTypes\\Thread", 0, ObTypeObjectType, &o)), "no Thread type");
    CHECK(o == PsThreadType, "\\ObjectTypes\\Thread is not PsThreadType");
    ObDereferenceObject(o);
    CHECK(NT_SUCCESS(ObReferenceObjectByName("\\device", 0, ObDirectoryObjectType, &o)), "no \\Device (case-insensitive)");
    ObDereferenceObject(o);

    /* A named, temporary event. */
    uint32_t events0 = ExEventObjectType->TotalObjects;
    KEVENT *ev;
    CHECK(NT_SUCCESS(ObCreateObject(ExEventObjectType, sizeof(KEVENT), (void **)&ev)), "create event");
    KeInitializeEvent(ev, NotificationEvent, false);
    CHECK(NT_SUCCESS(ObInsertObjectName(ev, "\\BaseNamedObjects\\SelfTestEvent", 0)), "insert name");
    HANDLE h;
    CHECK(NT_SUCCESS(ObCreateHandle(ev, &h)) && h && !(h & 3), "create handle");
    ObDereferenceObject(ev);                    /* the handle keeps it alive now */

    CHECK(NT_SUCCESS(ObReferenceObjectByName("\\BASENAMEDOBJECTS\\selftestevent", 0, ExEventObjectType, &o)),
          "lookup by name failed");
    CHECK(o == ev, "lookup found a different object");
    ObDereferenceObject(o);

    char path[128];
    ObQueryNameString(ev, path, sizeof(path));
    CHECK(!strcmp(path, "\\BaseNamedObjects\\SelfTestEvent"), "full name is '%s'", path);

    /* Symbolic links, including two in one path (\DosDevices -> \GLOBAL??). */
    CHECK(NT_SUCCESS(ObCreateSymbolicLink("\\GLOBAL??\\TESTDRV:", "\\BaseNamedObjects")), "create link");
    CHECK(NT_SUCCESS(ObReferenceObjectByName("\\DosDevices\\TestDrv:\\SelfTestEvent", 0, NULL, &o)),
          "lookup through two symbolic links failed");
    CHECK(o == ev, "link lookup found a different object");
    ObDereferenceObject(o);
    CHECK(NT_SUCCESS(ObReferenceObjectByName("\\GLOBAL??\\TESTDRV:", OBJ_OPENLINK, ObSymbolicLinkObjectType, &o)),
          "OBJ_OPENLINK did not return the link itself");
    ObMakeTemporaryObject(o);                   /* removes it from the namespace */
    ObDereferenceObject(o);
    CHECK(ObReferenceObjectByName("\\GLOBAL??\\TESTDRV:", 0, NULL, &o) == STATUS_OBJECT_NAME_NOT_FOUND,
          "temporary link still in the namespace");

    /* Name collisions and bad lookups. */
    KEVENT *ev2;
    ObCreateObject(ExEventObjectType, sizeof(KEVENT), (void **)&ev2);
    CHECK(ObInsertObjectName(ev2, "\\BaseNamedObjects\\selftestEVENT", 0) == STATUS_OBJECT_NAME_COLLISION,
          "duplicate name accepted");
    ObDereferenceObject(ev2);
    CHECK(ObReferenceObjectByName("\\NoSuchDir\\X", 0, NULL, &o) == STATUS_OBJECT_PATH_NOT_FOUND, "bad path");
    CHECK(ObReferenceObjectByName("\\BaseNamedObjects\\Nope", 0, NULL, &o) == STATUS_OBJECT_NAME_NOT_FOUND,
          "bad name");

    /* Handles: type checks, bad handles. */
    CHECK(ObReferenceObjectByHandle(h, PsThreadType, &o) == STATUS_OBJECT_TYPE_MISMATCH, "type not checked");
    CHECK(ObReferenceObjectByHandle(h + 400, NULL, &o) == STATUS_INVALID_HANDLE, "bogus handle accepted");
    CHECK(NT_SUCCESS(ObReferenceObjectByHandle(h, ExEventObjectType, &o)) && o == ev, "handle lookup");
    ObDereferenceObject(o);

    /* Closing the last handle removes the name and deletes the object. */
    CHECK(ObCloseHandle(h) == STATUS_SUCCESS, "close");
    CHECK(ObCloseHandle(h) == STATUS_INVALID_HANDLE, "double close accepted");
    CHECK(ObReferenceObjectByName("\\BaseNamedObjects\\SelfTestEvent", 0, NULL, &o) == STATUS_OBJECT_NAME_NOT_FOUND,
          "name still there after the last handle closed");
    CHECK(ExEventObjectType->TotalObjects == events0, "%u events left, expected %u",
          ExEventObjectType->TotalObjects, events0);

    /* A new type with a delete procedure. */
    OBJECT_TYPE *tt;
    CHECK(NT_SUCCESS(ObCreateObjectType("SelfTest", counting_delete, 'STst', &tt)), "create type");
    g_deleted = 0;
    void *x;
    ObCreateObject(tt, 64, &x);
    ObReferenceObject(x);
    ObDereferenceObject(x);
    CHECK(g_deleted == 0, "deleted while still referenced");
    ObDereferenceObject(x);
    CHECK(g_deleted == 1 && tt->TotalObjects == 0, "delete procedure not called");
    return true;
}
