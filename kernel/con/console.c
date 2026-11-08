/*
 * Kernel console multiplexer and kernel log.
 */
#include "console.h"
#include "hal.h"
#include "rtl.h"

uint64_t HalGetTimeNs(void);

void ConWrite(const char *s, size_t len)
{
    FbConWrite(s, len);
    HalSerialWrite(s, len);
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
    uint64_t ns = HalGetTimeNs();
    char stamp[32];
    int n = snprintf(stamp, sizeof(stamp), "[%5llu.%03llu] ",
                     (unsigned long long)(ns / 1000000000ull),
                     (unsigned long long)((ns / 1000000ull) % 1000));
    ConWrite(stamp, (size_t)n);

    va_list ap;
    va_start(ap, fmt);
    ConVPrint(fmt, ap);
    va_end(ap);
    ConWrite("\n", 1);
}
