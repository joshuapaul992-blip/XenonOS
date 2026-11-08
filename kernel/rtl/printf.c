/* SPDX-License-Identifier: MIT */
/*
 * Rtl - printf-style formatting.
 *
 * Supports: %d %i %u %x %X %o %p %s %c %%, flags "-0+ #", field width and
 * precision (including '*'), and the length modifiers hh h l ll z t j.
 */
#include "rtl.h"

struct out {
    char *buf;
    size_t size;
    size_t pos;
};

static void put(struct out *o, char c)
{
    if (o->pos + 1 < o->size)
        o->buf[o->pos] = c;
    o->pos++;
}

static void pad(struct out *o, char c, int n)
{
    while (n-- > 0)
        put(o, c);
}

static void emit_number(struct out *o, uint64_t v, bool neg, unsigned base, bool upper,
                        int width, int prec, bool left, bool zero, char sign, const char *prefix)
{
    char tmp[24];
    int len = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    if (v == 0 && prec != 0)
        tmp[len++] = '0';
    while (v) {
        tmp[len++] = digits[v % base];
        v /= base;
    }
    int zeros = prec > len ? prec - len : 0;
    int plen = (int)strlen(prefix) + ((neg || sign) ? 1 : 0);
    int total = len + zeros + plen;

    if (zero && !left && prec < 0) {
        zeros += width > total ? width - total : 0;
        total = width > total ? width : total;
    }
    if (!left)
        pad(o, ' ', width - total);
    if (neg)
        put(o, '-');
    else if (sign)
        put(o, sign);
    for (const char *p = prefix; *p; p++)
        put(o, *p);
    pad(o, '0', zeros);
    while (len)
        put(o, tmp[--len]);
    if (left)
        pad(o, ' ', width - total);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct out o = { buf, size, 0 };

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            put(&o, *fmt);
            continue;
        }
        fmt++;
        bool left = false, zero = false, alt = false;
        char sign = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = true;
            else if (*fmt == '0') zero = true;
            else if (*fmt == '+') sign = '+';
            else if (*fmt == ' ') { if (!sign) sign = ' '; }
            else if (*fmt == '#') alt = true;
            else break;
        }
        int width = 0, prec = -1;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) { left = true; width = -width; }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + (*fmt++ - '0');
        }
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9')
                    prec = prec * 10 + (*fmt++ - '0');
            }
        }
        int lng = 0;   /* 0=int, 1=long, 2=long long, -1=short, -2=char */
        for (;; fmt++) {
            if (*fmt == 'l') lng++;
            else if (*fmt == 'h') lng--;
            else if (*fmt == 'z' || *fmt == 't' || *fmt == 'j') lng = 2;
            else break;
        }

        switch (*fmt) {
        case 'd':
        case 'i': {
            int64_t v;
            if (lng >= 1) v = lng == 1 ? va_arg(ap, long) : va_arg(ap, long long);
            else v = va_arg(ap, int);
            if (lng == -1) v = (short)v;
            if (lng <= -2) v = (signed char)v;
            uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
            emit_number(&o, u, v < 0, 10, false, width, prec, left, zero, sign, "");
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            uint64_t v;
            if (lng >= 1) v = lng == 1 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned long long);
            else v = va_arg(ap, unsigned int);
            if (lng == -1) v = (unsigned short)v;
            if (lng <= -2) v = (unsigned char)v;
            unsigned base = *fmt == 'u' ? 10 : *fmt == 'o' ? 8 : 16;
            const char *prefix = "";
            if (alt && v && base == 16) prefix = *fmt == 'X' ? "0X" : "0x";
            if (alt && base == 8) prefix = "0";
            emit_number(&o, v, false, base, *fmt == 'X', width, prec, left, zero, 0, prefix);
            break;
        }
        case 'p': {
            uint64_t v = (uint64_t)(uintptr_t)va_arg(ap, void *);
            emit_number(&o, v, false, 16, false, width ? width : 18, 16, left, false, 0, "0x");
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            int len = (int)strnlen(s, prec >= 0 ? (size_t)prec : (size_t)-1);
            if (!left)
                pad(&o, ' ', width - len);
            for (int i = 0; i < len; i++)
                put(&o, s[i]);
            if (left)
                pad(&o, ' ', width - len);
            break;
        }
        case 'c':
            if (!left)
                pad(&o, ' ', width - 1);
            put(&o, (char)va_arg(ap, int));
            if (left)
                pad(&o, ' ', width - 1);
            break;
        case '%':
            put(&o, '%');
            break;
        case 0:
            fmt--;
            break;
        default:
            put(&o, '%');
            put(&o, *fmt);
            break;
        }
    }
    if (size)
        buf[o.pos < size ? o.pos : size - 1] = 0;
    return (int)o.pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
