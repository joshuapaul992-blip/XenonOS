/*
 * Ex - kernel log ring buffer (what "dmesg" shows).
 *
 * Every DbgPrint line goes here as well as to the screen and COM1. The lock
 * is taken at HIGH_LEVEL, so interrupt handlers can log too.
 */
#include "ex.h"
#include "ke.h"
#include "rtl.h"

static char g_log[EX_LOG_SIZE];
static uint64_t g_total;          /* bytes ever written; write position = g_total % size */
static KSPIN_LOCK g_lock;

void ExLogWrite(const char *s, size_t len)
{
    KIRQL old = KeAcquireSpinLockRaiseToHigh(&g_lock);
    for (size_t i = 0; i < len; i++)
        g_log[(g_total + i) % EX_LOG_SIZE] = s[i];
    g_total += len;
    KeReleaseSpinLock(&g_lock, old);
}

size_t ExLogRead(char *buf, size_t size)
{
    KIRQL old = KeAcquireSpinLockRaiseToHigh(&g_lock);
    uint64_t avail = g_total < EX_LOG_SIZE ? g_total : EX_LOG_SIZE;
    uint64_t start = g_total - avail;
    if (avail > size) {
        start += avail - size;
        avail = size;
    }
    for (uint64_t i = 0; i < avail; i++)
        buf[i] = g_log[(start + i) % EX_LOG_SIZE];
    KeReleaseSpinLock(&g_lock, old);
    return (size_t)avail;
}

uint64_t ExLogTotal(void)
{
    return g_total;
}
