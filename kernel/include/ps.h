/*
 * Ps - processes and threads (executive layer over the Ke thread).
 *
 * Phase 1.2 has two processes: Idle (PID 0) and System (PID 4), which owns
 * every kernel thread. User processes arrive in Phase 1.5.
 */
#pragma once
#include "ke.h"
#include "ob.h"

typedef struct KPROCESS {
    DISPATCHER_HEADER Header;          /* signaled when the process exits */
} KPROCESS;

typedef struct EPROCESS {
    KPROCESS Pcb;
    uint64_t UniqueProcessId;
    uint64_t ParentProcessId;
    char ImageFileName[16];
    HANDLE_TABLE HandleTable;
    LIST_ENTRY ProcessListEntry;
    LIST_ENTRY ThreadListHead;
    uint32_t ActiveThreads;
    uint64_t CreateTime;               /* uptime ns */
} EPROCESS;

typedef struct ETHREAD {
    KTHREAD Tcb;                       /* must be first: the thread object is waitable */
    EPROCESS *ThreadsProcess;
    uint64_t CreateTime;
} ETHREAD;

extern OBJECT_TYPE *PsProcessType, *PsThreadType;
extern EPROCESS *PsInitialSystemProcess;
extern EPROCESS *PsIdleProcess;

void PsInitSystem(void *boot_stack_top, size_t boot_stack_size);

NTSTATUS PsCreateSystemThread(HANDLE *handle, KSTART_ROUTINE routine, void *context, int priority,
                              const char *name, ETHREAD **thread);
NORETURN void PsTerminateSystemThread(NTSTATUS status);

static inline ETHREAD *PsGetCurrentThread(void) { return (ETHREAD *)KeGetCurrentThread(); }
EPROCESS *PsGetCurrentProcess(void);
NTSTATUS PsLookupThreadByThreadId(uint64_t tid, ETHREAD **thread);   /* referenced */
void PsEnumerateProcesses(void (*cb)(EPROCESS *p, void *ctx), void *ctx);
