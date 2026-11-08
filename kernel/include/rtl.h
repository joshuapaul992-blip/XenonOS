/*
 * Rtl - kernel runtime library (strings, memory, formatting).
 */
#pragma once
#include "xenon.h"

void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strchr(const char *s, int c);
char  *strstr(const char *haystack, const char *needle);
size_t strlcpy(char *dst, const char *src, size_t size);

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...) PRINTF_LIKE(3, 4);

/* Find "key=value" in a space-separated command line. Copies value into out. */
bool RtlGetCmdlineOption(const char *cmdline, const char *key, char *out, size_t outsize);
int    tolower(int c);
int    toupper(int c);
int    strcasecmp(const char *a, const char *b);
int    strncasecmp(const char *a, const char *b, size_t n);
char  *strrchr(const char *s, int c);
int    atoi(const char *s);
