/* SPDX-License-Identifier: MIT */
/*
 * Doubly linked lists (NT style LIST_ENTRY).
 */
#pragma once
#include "xenon.h"

typedef struct LIST_ENTRY {
    struct LIST_ENTRY *Flink;
    struct LIST_ENTRY *Blink;
} LIST_ENTRY;

#define CONTAINING_RECORD(addr, type, field) ((type *)((char *)(addr) - offsetof(type, field)))

static inline void InitializeListHead(LIST_ENTRY *h) { h->Flink = h->Blink = h; }
static inline bool IsListEmpty(const LIST_ENTRY *h) { return h->Flink == h; }

static inline void InsertTailList(LIST_ENTRY *h, LIST_ENTRY *e)
{
    e->Flink = h;
    e->Blink = h->Blink;
    h->Blink->Flink = e;
    h->Blink = e;
}

static inline void InsertHeadList(LIST_ENTRY *h, LIST_ENTRY *e)
{
    e->Flink = h->Flink;
    e->Blink = h;
    h->Flink->Blink = e;
    h->Flink = e;
}

/* Insert e before 'before' (which may be the list head = append). */
static inline void InsertBeforeList(LIST_ENTRY *before, LIST_ENTRY *e)
{
    InsertTailList(before, e);
}

static inline bool RemoveEntryList(LIST_ENTRY *e)
{
    LIST_ENTRY *f = e->Flink, *b = e->Blink;
    b->Flink = f;
    f->Blink = b;
    e->Flink = e->Blink = e;
    return f == b;      /* true if the list is now empty */
}

static inline LIST_ENTRY *RemoveHeadList(LIST_ENTRY *h)
{
    LIST_ENTRY *e = h->Flink;
    RemoveEntryList(e);
    return e;
}

static inline bool IsEntryLinked(const LIST_ENTRY *e) { return e->Flink != e; }
