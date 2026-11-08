/*
 * Ke - dispatcher objects, waiting and kernel timers.
 *
 * Every waitable object starts with a DISPATCHER_HEADER: a signal state and
 * a list of wait blocks of the threads waiting for it. When an object becomes
 * signaled, KiWaitTest walks that list and satisfies as many waits as the
 * object allows (one for a synchronization event, "count" for a semaphore,
 * all for a notification event). Satisfying a wait also "consumes" the object
 * where that applies (resets the event, takes a semaphore unit, gives the
 * mutex an owner) in the same step, so waits are atomic.
 *
 * Timers live in one list sorted by due time. The clock interrupt compares
 * the first due time with the clock and queues a DPC that expires timers.
 */
#include "ke.h"
#include "hal.h"
#include "rtl.h"
#include "mm.h"

static KSPIN_LOCK g_lock;                 /* dispatcher database */
static LIST_ENTRY g_timer_list;
static KSPIN_LOCK g_timer_lock;           /* taken at HIGH_LEVEL: the clock reads g_next_due */
static volatile uint64_t g_next_due = UINT64_MAX;
static KDPC g_timer_dpc;

#define LOCK()   KIRQL old_irql_ = KeAcquireSpinLock(&g_lock)
#define UNLOCK() KeReleaseSpinLock(&g_lock, old_irql_)

static void init_header(DISPATCHER_HEADER *h, uint8_t type, int32_t state)
{
    h->Type = type;
    h->SignalState = state;
    InitializeListHead(&h->WaitListHead);
}

/* ---- Satisfying waits --------------------------------------------------- */

static bool signaled_for(DISPATCHER_HEADER *h, KTHREAD *t)
{
    if (h->Type == MutantObject)
        return h->SignalState > 0 || ((KMUTANT *)h)->OwnerThread == t;
    return h->SignalState > 0;
}

/* Consume the object on behalf of thread t. Returns true if a mutex was abandoned. */
static bool consume(DISPATCHER_HEADER *h, KTHREAD *t)
{
    switch (h->Type) {
    case SynchronizationEvent:
    case SynchronizationTimer:
        h->SignalState = 0;
        return false;
    case SemaphoreObject:
        h->SignalState--;
        return false;
    case MutantObject: {
        KMUTANT *m = (KMUTANT *)h;
        bool abandoned = false;
        if (m->OwnerThread != t) {
            m->OwnerThread = t;
            InsertTailList(&t->MutantListHead, &m->MutantListEntry);
            abandoned = m->Abandoned;
            m->Abandoned = false;
        }
        h->SignalState--;
        return abandoned;
    }
    default:
        return false;            /* notification objects stay signaled */
    }
}

void KiUnwaitThread(KTHREAD *t, NTSTATUS status)
{
    for (unsigned i = 0; i < t->WaitCount; i++)
        RemoveEntryList(&t->WaitBlocks[i].WaitListEntry);
    t->WaitCount = 0;
    KiRemoveTimer(&t->Timer);
    t->WaitStatus = status;
    KiReadyThread(t);
}

void KiWaitTest(DISPATCHER_HEADER *obj)
{
    LIST_ENTRY *e = obj->WaitListHead.Flink;
    while (e != &obj->WaitListHead && obj->SignalState > 0) {
        KWAIT_BLOCK *wb = CONTAINING_RECORD(e, KWAIT_BLOCK, WaitListEntry);
        KTHREAD *t = wb->Thread;
        if (wb->WaitType == WaitAny) {
            NTSTATUS st = (consume(obj, t) ? STATUS_ABANDONED_WAIT_0 : STATUS_WAIT_0) + wb->WaitKey;
            KiUnwaitThread(t, st);
            e = obj->WaitListHead.Flink;
            continue;
        }
        bool all = true;
        for (unsigned i = 0; i < t->WaitCount && all; i++)
            all = signaled_for(t->WaitBlocks[i].Object, t);
        if (all) {
            bool abandoned = false;
            for (unsigned i = 0; i < t->WaitCount; i++)
                abandoned |= consume(t->WaitBlocks[i].Object, t);
            KiUnwaitThread(t, abandoned ? STATUS_ABANDONED_WAIT_0 : STATUS_WAIT_0);
            e = obj->WaitListHead.Flink;
            continue;
        }
        e = e->Flink;
    }
}

/* ---- Waiting ------------------------------------------------------------ */

static uint64_t absolute_due(int64_t timeout_ns)
{
    if (timeout_ns < 0)
        return KeQueryUptimeNs() + (uint64_t)(-timeout_ns);
    return (uint64_t)timeout_ns;
}

NTSTATUS KeWaitForMultipleObjects(unsigned count, void *objects[], WAIT_TYPE type, int64_t timeout_ns)
{
    if (count > MAXIMUM_WAIT_OBJECTS)
        return STATUS_INVALID_PARAMETER;
    KTHREAD *t = KeGetCurrentThread();
    KIRQL entry = KeGetCurrentIrql();
    if (entry > DISPATCH_LEVEL || (entry == DISPATCH_LEVEL && timeout_ns != 0))
        KeBugCheckEx(IRQL_NOT_LESS_OR_EQUAL, entry, (uint64_t)objects, timeout_ns != 0, 5);

    LOCK();
    NTSTATUS st;
    DISPATCHER_HEADER **objs = (DISPATCHER_HEADER **)objects;

    if (type == WaitAny) {
        for (unsigned i = 0; i < count; i++) {
            if (signaled_for(objs[i], t)) {
                st = (consume(objs[i], t) ? STATUS_ABANDONED_WAIT_0 : STATUS_WAIT_0) + (NTSTATUS)i;
                UNLOCK();
                return st;
            }
        }
    } else if (count > 0) {
        bool all = true;
        for (unsigned i = 0; i < count && all; i++)
            all = signaled_for(objs[i], t);
        if (all) {
            bool abandoned = false;
            for (unsigned i = 0; i < count; i++)
                abandoned |= consume(objs[i], t);
            UNLOCK();
            return abandoned ? STATUS_ABANDONED_WAIT_0 : STATUS_WAIT_0;
        }
    }

    if (timeout_ns == 0) {
        UNLOCK();
        return STATUS_TIMEOUT;
    }
    uint64_t due = 0;
    if (timeout_ns != KE_INFINITE) {
        due = absolute_due(timeout_ns);
        if (due <= KeQueryUptimeNs()) {
            UNLOCK();
            return STATUS_TIMEOUT;
        }
    } else if (count == 0) {
        UNLOCK();
        return STATUS_INVALID_PARAMETER;       /* would sleep forever */
    }

    KWAIT_BLOCK *blocks = t->WaitBlockFill;
    if (count > THREAD_WAIT_OBJECTS) {
        /* Big waits get a temporary array (allocated at DISPATCH_LEVEL, which
           the non-paged pool allows). */
        blocks = ExAllocatePoolWithTag(NonPagedPool, count * sizeof(KWAIT_BLOCK), 'KeWb');
        if (!blocks) {
            UNLOCK();
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    t->WaitBlocks = blocks;
    for (unsigned i = 0; i < count; i++) {
        KWAIT_BLOCK *wb = &blocks[i];
        wb->Thread = t;
        wb->Object = objs[i];
        wb->WaitKey = (uint16_t)i;
        wb->WaitType = (uint8_t)type;
        InsertTailList(&objs[i]->WaitListHead, &wb->WaitListEntry);
    }
    t->WaitCount = count;
    t->WaitType = (uint8_t)type;
    t->WaitStatus = STATUS_PENDING;
    if (timeout_ns != KE_INFINITE) {
        t->Timer.WaitThread = t;
        KiInsertTimer(&t->Timer, due);
    }
    t->State = Waiting;
    KeReleaseSpinLockFromDpcLevel(&g_lock);
    KiSwapThread();
    st = t->WaitStatus;
    if (blocks != t->WaitBlockFill)
        ExFreePoolWithTag(blocks, 'KeWb');
    t->WaitBlocks = t->WaitBlockFill;
    KeLowerIrql(old_irql_);
    return st;
}

NTSTATUS KeWaitForSingleObject(void *object, int64_t timeout_ns)
{
    void *objs[1] = { object };
    return KeWaitForMultipleObjects(1, objs, WaitAny, timeout_ns);
}

NTSTATUS KeDelayExecutionThread(int64_t interval_ns)
{
    if (interval_ns == 0) {
        KeYieldExecution();
        return STATUS_SUCCESS;
    }
    NTSTATUS st = KeWaitForMultipleObjects(0, NULL, WaitAny, interval_ns);
    return st == STATUS_TIMEOUT ? STATUS_SUCCESS : st;
}

/* ---- Events ------------------------------------------------------------- */

void KeInitializeEvent(KEVENT *e, KOBJECTS type, bool state)
{
    init_header(&e->Header, (uint8_t)type, state ? 1 : 0);
}

int32_t KeSetEvent(KEVENT *e)
{
    LOCK();
    int32_t prev = e->Header.SignalState;
    e->Header.SignalState = 1;
    KiWaitTest(&e->Header);
    UNLOCK();
    return prev;
}

int32_t KeResetEvent(KEVENT *e)
{
    LOCK();
    int32_t prev = e->Header.SignalState;
    e->Header.SignalState = 0;
    UNLOCK();
    return prev;
}

void KeClearEvent(KEVENT *e)
{
    e->Header.SignalState = 0;
}

int32_t KePulseEvent(KEVENT *e)
{
    LOCK();
    int32_t prev = e->Header.SignalState;
    e->Header.SignalState = 1;
    KiWaitTest(&e->Header);
    e->Header.SignalState = 0;
    UNLOCK();
    return prev;
}

int32_t KeReadStateEvent(KEVENT *e)
{
    return e->Header.SignalState;
}

/* ---- Mutexes ------------------------------------------------------------ */

void KeInitializeMutex(KMUTEX *m)
{
    init_header(&m->Header, MutantObject, 1);
    InitializeListHead(&m->MutantListEntry);
    m->OwnerThread = NULL;
    m->Abandoned = false;
}

NTSTATUS KeReleaseMutant(KMUTANT *m, bool abandoned)
{
    LOCK();
    KTHREAD *t = KeGetCurrentThread();
    if (!abandoned && m->OwnerThread != t) {
        UNLOCK();
        return STATUS_MUTANT_NOT_OWNED;
    }
    int32_t prev = m->Header.SignalState;
    if (abandoned) {
        m->Header.SignalState = 1;
        m->Abandoned = true;
    } else {
        m->Header.SignalState++;
    }
    if (m->Header.SignalState == 1) {
        if (m->OwnerThread)
            RemoveEntryList(&m->MutantListEntry);
        m->OwnerThread = NULL;
        KiWaitTest(&m->Header);
    }
    UNLOCK();
    return prev;
}

int32_t KeReleaseMutex(KMUTEX *m)
{
    NTSTATUS st = KeReleaseMutant(m, false);
    if (st == STATUS_MUTANT_NOT_OWNED)
        KeBugCheckEx(THREAD_NOT_MUTEX_OWNER, (uint64_t)m, (uint64_t)KeGetCurrentThread(),
                     (uint64_t)m->OwnerThread, 0);
    return st;
}

/* ---- Semaphores --------------------------------------------------------- */

void KeInitializeSemaphore(KSEMAPHORE *s, int32_t count, int32_t limit)
{
    init_header(&s->Header, SemaphoreObject, count);
    s->Limit = limit;
}

NTSTATUS KeReleaseSemaphore(KSEMAPHORE *s, int32_t adjustment, int32_t *previous)
{
    LOCK();
    int32_t prev = s->Header.SignalState;
    if (adjustment <= 0 || prev + adjustment > s->Limit || prev + adjustment < prev) {
        UNLOCK();
        return STATUS_SEMAPHORE_LIMIT_EXCEEDED;
    }
    s->Header.SignalState = prev + adjustment;
    KiWaitTest(&s->Header);
    UNLOCK();
    if (previous)
        *previous = prev;
    return STATUS_SUCCESS;
}

/* ---- Timers ------------------------------------------------------------- */

void KeInitializeTimerEx(KTIMER *t, KOBJECTS type)
{
    init_header(&t->Header, (uint8_t)type, 0);
    InitializeListHead(&t->TimerListEntry);
    t->Inserted = false;
    t->Dpc = NULL;
    t->PeriodMs = 0;
    t->WaitThread = NULL;
}

void KeInitializeTimer(KTIMER *t)
{
    KeInitializeTimerEx(t, NotificationTimer);
}

void KiInsertTimer(KTIMER *t, uint64_t due)
{
    KIRQL old = KeAcquireSpinLockRaiseToHigh(&g_timer_lock);
    if (t->Inserted)
        RemoveEntryList(&t->TimerListEntry);
    t->DueTime = due;
    t->Inserted = true;
    LIST_ENTRY *e = g_timer_list.Flink;
    while (e != &g_timer_list && CONTAINING_RECORD(e, KTIMER, TimerListEntry)->DueTime <= due)
        e = e->Flink;
    InsertBeforeList(e, &t->TimerListEntry);
    g_next_due = CONTAINING_RECORD(g_timer_list.Flink, KTIMER, TimerListEntry)->DueTime;
    KeReleaseSpinLock(&g_timer_lock, old);
}

bool KiRemoveTimer(KTIMER *t)
{
    KIRQL old = KeAcquireSpinLockRaiseToHigh(&g_timer_lock);
    bool was = t->Inserted;
    if (was) {
        RemoveEntryList(&t->TimerListEntry);
        t->Inserted = false;
        g_next_due = IsListEmpty(&g_timer_list) ? UINT64_MAX
                   : CONTAINING_RECORD(g_timer_list.Flink, KTIMER, TimerListEntry)->DueTime;
    }
    KeReleaseSpinLock(&g_timer_lock, old);
    return was;
}

bool KeSetTimerEx(KTIMER *t, int64_t due_ns, uint32_t period_ms, KDPC *dpc)
{
    LOCK();
    bool was = KiRemoveTimer(t);
    t->Header.SignalState = 0;
    t->Dpc = dpc;
    t->PeriodMs = period_ms;
    t->WaitThread = NULL;
    uint64_t due = absolute_due(due_ns);
    KiInsertTimer(t, due);
    UNLOCK();
    /* Already due: let the expiry DPC handle it right away. */
    if (due <= KeQueryUptimeNs())
        KeInsertQueueDpc(&g_timer_dpc, NULL, NULL);
    return was;
}

bool KeSetTimer(KTIMER *t, int64_t due_ns, KDPC *dpc)
{
    return KeSetTimerEx(t, due_ns, 0, dpc);
}

bool KeCancelTimer(KTIMER *t)
{
    LOCK();
    bool was = KiRemoveTimer(t);
    UNLOCK();
    return was;
}

bool KeReadStateTimer(KTIMER *t)
{
    return t->Header.SignalState > 0;
}

/* Runs at DISPATCH_LEVEL: expire every timer whose due time has passed. */
static void timer_expiration_dpc(KDPC *dpc, void *ctx, void *a1, void *a2)
{
    UNUSED(dpc); UNUSED(ctx); UNUSED(a1); UNUSED(a2);
    KeAcquireSpinLockAtDpcLevel(&g_lock);
    uint64_t now = KeQueryUptimeNs();
    for (;;) {
        KIRQL old = KeAcquireSpinLockRaiseToHigh(&g_timer_lock);
        if (IsListEmpty(&g_timer_list) ||
            CONTAINING_RECORD(g_timer_list.Flink, KTIMER, TimerListEntry)->DueTime > now) {
            KeReleaseSpinLock(&g_timer_lock, old);
            break;
        }
        KTIMER *t = CONTAINING_RECORD(RemoveHeadList(&g_timer_list), KTIMER, TimerListEntry);
        t->Inserted = false;
        g_next_due = IsListEmpty(&g_timer_list) ? UINT64_MAX
                   : CONTAINING_RECORD(g_timer_list.Flink, KTIMER, TimerListEntry)->DueTime;
        KeReleaseSpinLock(&g_timer_lock, old);

        if (t->WaitThread) {
            KTHREAD *th = t->WaitThread;
            t->WaitThread = NULL;
            if (th->State == Waiting)
                KiUnwaitThread(th, STATUS_TIMEOUT);
            continue;
        }
        t->Header.SignalState = 1;
        KiWaitTest(&t->Header);
        if (t->PeriodMs) {
            uint64_t next = t->DueTime + (uint64_t)t->PeriodMs * 1000000;
            if (next <= now)
                next = now + (uint64_t)t->PeriodMs * 1000000;   /* fell behind: don't burst */
            KiInsertTimer(t, next);
        }
        if (t->Dpc)
            KeInsertQueueDpc(t->Dpc, (void *)(uintptr_t)now, NULL);
    }
    KeReleaseSpinLockFromDpcLevel(&g_lock);
}

void KiInitializeTimers(void)
{
    InitializeListHead(&g_timer_list);
    KeInitializeDpc(&g_timer_dpc, timer_expiration_dpc, NULL);
}

void KiCheckTimers(void)
{
    if (HalGetTimeNs() >= g_next_due)
        KeInsertQueueDpc(&g_timer_dpc, NULL, NULL);
}
