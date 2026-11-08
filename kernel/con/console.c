/*
 * Kernel console multiplexer and kernel log.
 *
 * Output goes to the framebuffer console and COM1 under a lock taken at
 * DISPATCH_LEVEL, so lines from different threads do not interleave.
 * Interrupt handlers (above DISPATCH_LEVEL) must not wait for that lock:
 * their DbgPrint lines go to the log buffer and COM1 only. The stop screen
 * bypasses the lock entirely.
 */
#include "console.h"
#include "hal.h"
#include "ke.h"
#include "ex.h"
#include "rtl.h"

static KSPIN_LOCK g_lock;
volatile bool KiConsoleBypassLock;

void ConWrite(const char *s, size_t len)
{
    if (KiConsoleBypassLock) {
        FbConWrite(s, len);
        HalSerialWrite(s, len);
        return;
    }
    if (KeGetCurrentIrql() > DISPATCH_LEVEL) {
        HalSerialWrite(s, len);
        return;
    }
    KIRQL old = KeAcquireSpinLock(&g_lock);
    FbConWrite(s, len);
    HalSerialWrite(s, len);
    KeReleaseSpinLock(&g_lock, old);
}

void ConVPrint(const char *fmt, va_list ap)
{
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n > (int)sizeof(buf) - 1)
        n = sizeof(buf) - 1;
    ConWrite(buf, (size_t)n);
}

void ConPrint(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ConVPrint(fmt, ap);
    va_end(ap);
}

void DbgPrint(const char *fmt, ...)
{
    char line[600];
    uint64_t ns = HalGetTimeNs();
    int n = snprintf(line, sizeof(line), "[%5llu.%03llu] ", (unsigned long long)(ns / 1000000000ull),
                     (unsigned long long)((ns / 1000000ull) % 1000));
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof(line) - n - 1, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(line) - 2)
        n = sizeof(line) - 2;
    line[n++] = '\n';
    line[n] = 0;
    ExLogWrite(line, (size_t)n);
    ConWrite(line, (size_t)n);
}
