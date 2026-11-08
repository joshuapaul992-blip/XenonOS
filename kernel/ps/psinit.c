/* SPDX-License-Identifier: MIT */
/*
 * Ps - process and thread objects.
 *
 * Process and thread IDs come from one counter in steps of 4, as on NT:
 * Idle is PID 0, System is PID 4. A thread object stays alive while the
 * thread runs (one reference) and while handles or pointers to it exist.
 * When a thread exits, the scheduler hands it to the reaper, which frees its
 * kernel stack and drops the "running" reference.
 */
#include "ps.h"
#include "mm.h"
#include "rtl.h"
#include "console.h"

#define SYSTEM_THREAD_STACK (32 * 1024)

OBJECT_TYPE *PsProcessType, *PsThreadType;
EPROCESS *PsInitialSystemProcess;
EPROCESS *PsIdleProcess;

static LIST_ENTRY g_process_list;
static KSPIN_LOCK g_lock;
static volatile uint64_t g_next_cid = 8;

void MmFreeKernelStack(void *top, size_t size);

static uint64_t new_cid(void)
{
    return __atomic_fetch_add(&g_next_cid, 4, __ATOMIC_RELAXED);
}

EPROCESS *PsGetCurrentProcess(void)
{
    ETHREAD *t = PsGetCurrentThread();
    return t ? t->ThreadsProcess : NULL;
}

static void delete_thread(void *object)
{
    UNUSED(object);
}

static void delete_process(void *object)
{
    EPROCESS *p = object;
    ObKillHandleTable(&p->HandleTable);
    KIRQL old = KeAcquireSpinLock(&g_lock);
    RemoveEntryList(&p->ProcessListEntry);
    KeReleaseSpinLock(&g_lock, old);
}

static NTSTATUS create_process(const char *name, uint64_t pid, EPROCESS **out)
{
    EPROCESS *p;
    NTSTATUS st = ObCreateObject(PsProcessType, sizeof(EPROCESS), (void **)&p);
    if (!NT_SUCCESS(st))
        return st;
    p->Pcb.Header.Type = ProcessObject;
    InitializeListHead(&p->Pcb.Header.WaitListHead);
    p->UniqueProcessId = pid;
    strlcpy(p->ImageFileName, name, sizeof(p->ImageFileName));
    ObInitializeHandleTable(&p->HandleTable);
    InitializeListHead(&p->ThreadListHead);
    p->CreateTime = KeQueryUptimeNs();
    KIRQL old = KeAcquireSpinLock(&g_lock);
    InsertTailList(&g_process_list, &p->ProcessListEntry);
    KeReleaseSpinLock(&g_lock, old);
    *out = p;
    return STATUS_SUCCESS;
}

static void attach_thread(ETHREAD *t, EPROCESS *p)
{
    t->ThreadsProcess = p;
    t->Tcb.Process = &p->Pcb;
    t->CreateTime = KeQueryUptimeNs();
    KIRQL old = KeAcquireSpinLock(&g_lock);
    InsertTailList(&p->ThreadListHead, &t->Tcb.ProcessThreadEntry);
    p->ActiveThreads++;
    KeReleaseSpinLock(&g_lock, old);
}

/* Runs at DISPATCH_LEVEL right after the scheduler has switched away from a
   thread that exited. */
static void reap_thread(KTHREAD *kt)
{
    ETHREAD *t = (ETHREAD *)kt;
    KIRQL old = KeAcquireSpinLock(&g_lock);
    RemoveEntryList(&kt->ProcessThreadEntry);
    if (t->ThreadsProcess)
        t->ThreadsProcess->ActiveThreads--;
    KeReleaseSpinLock(&g_lock, old);
    if (kt->StackTop && kt->StackSize)
        MmFreeKernelStack(kt->StackTop, kt->StackSize);
    kt->StackTop = NULL;
    ObDereferenceObject(t);
}

NTSTATUS PsCreateSystemThread(HANDLE *handle, KSTART_ROUTINE routine, void *context, int priority,
                              const char *name, ETHREAD **out)
{
    ETHREAD *t;
    NTSTATUS st = ObCreateObject(PsThreadType, sizeof(ETHREAD), (void **)&t);
    if (!NT_SUCCESS(st))
        return st;
    void *stack = MmCreateKernelStack(SYSTEM_THREAD_STACK);
    if (!stack) {
        ObDereferenceObject(t);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    KeInitializeThread(&t->Tcb, stack, SYSTEM_THREAD_STACK, routine, context, priority, name);
    t->Tcb.Cid = new_cid();
    attach_thread(t, PsInitialSystemProcess);

    /* References: one for the running thread (dropped by the reaper), plus
       one for the handle and one for *out if asked for. */
    if (handle) {
        st = ObCreateHandle(t, handle);
        if (!NT_SUCCESS(st)) {
            KIRQL old = KeAcquireSpinLock(&g_lock);
            RemoveEntryList(&t->Tcb.ProcessThreadEntry);
            PsInitialSystemProcess->ActiveThreads--;
            KeReleaseSpinLock(&g_lock, old);
            MmFreeKernelStack(stack, SYSTEM_THREAD_STACK);
            ObDereferenceObject(t);
            return st;
        }
    }
    if (out) {
        ObReferenceObject(t);
        *out = t;
    }
    KeReadyThread(&t->Tcb);
    return STATUS_SUCCESS;
}

NORETURN void PsTerminateSystemThread(NTSTATUS status)
{
    KeTerminateThread(status);
}

typedef struct { uint64_t Tid; ETHREAD *Found; } FIND_CTX;
static void find_cb(KTHREAD *t, void *ctx)
{
    FIND_CTX *f = ctx;
    if (t->Cid == f->Tid && !f->Found && t->State != Terminated) {
        f->Found = (ETHREAD *)t;
        ObReferenceObject(t);
    }
}

NTSTATUS PsLookupThreadByThreadId(uint64_t tid, ETHREAD **thread)
{
    FIND_CTX f = { tid, NULL };
    KeEnumerateThreads(find_cb, &f);
    if (!f.Found)
        return STATUS_INVALID_PARAMETER;
    *thread = f.Found;
    return STATUS_SUCCESS;
}

void PsEnumerateProcesses(void (*cb)(EPROCESS *p, void *ctx), void *ctx)
{
    KIRQL old = KeAcquireSpinLock(&g_lock);
    for (LIST_ENTRY *e = g_process_list.Flink; e != &g_process_list; e = e->Flink)
        cb(CONTAINING_RECORD(e, EPROCESS, ProcessListEntry), ctx);
    KeReleaseSpinLock(&g_lock, old);
}

void PsInitSystem(void *boot_stack_top, size_t boot_stack_size)
{
    InitializeListHead(&g_process_list);
    if (!NT_SUCCESS(ObCreateObjectType("Process", delete_process, 'Proc', &PsProcessType)) ||
        !NT_SUCCESS(ObCreateObjectType("Thread", delete_thread, 'Thre', &PsThreadType)) ||
        !NT_SUCCESS(create_process("Idle", 0, &PsIdleProcess)) ||
        !NT_SUCCESS(create_process("System", 4, &PsInitialSystemProcess)))
        KeBugCheckEx(PHASE1_INITIALIZATION_FAILED, 1, 0, 0, 0);

    ETHREAD *init, *idle;
    void *idle_stack = MmCreateKernelStack(SYSTEM_THREAD_STACK);
    if (!NT_SUCCESS(ObCreateObject(PsThreadType, sizeof(ETHREAD), (void **)&init)) ||
        !NT_SUCCESS(ObCreateObject(PsThreadType, sizeof(ETHREAD), (void **)&idle)) || !idle_stack)
        KeBugCheckEx(PHASE1_INITIALIZATION_FAILED, 2, 0, 0, 0);

    KiInitializeScheduler(&init->Tcb, &idle->Tcb, idle_stack, SYSTEM_THREAD_STACK);
    init->Tcb.StackTop = boot_stack_top;
    init->Tcb.StackSize = boot_stack_size;
    init->Tcb.Cid = new_cid();
    idle->Tcb.Cid = 0;
    attach_thread(init, PsInitialSystemProcess);
    attach_thread(idle, PsIdleProcess);
    KiSetKernelStack((uint64_t)boot_stack_top);
    KiThreadReaper = reap_thread;
}
