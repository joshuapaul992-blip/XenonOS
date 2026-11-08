/* SPDX-License-Identifier: MIT */
/*
 * XenonOS kernel - common definitions.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

#define ARRAY_SIZE(a)     (sizeof(a) / sizeof((a)[0]))
#define ALIGN_UP(x, a)    (((x) + ((a) - 1)) & ~((uint64_t)(a) - 1))
#define ALIGN_DOWN(x, a)  ((x) & ~((uint64_t)(a) - 1))
#define MIN(a, b)         ((a) < (b) ? (a) : (b))
#define MAX(a, b)         ((a) > (b) ? (a) : (b))

#define NORETURN          __attribute__((noreturn))
#define PACKED            __attribute__((packed))
#define USED              __attribute__((used))
#define SECTION(s)        __attribute__((section(s)))
#define PRINTF_LIKE(f, a) __attribute__((format(printf, f, a)))
#define UNUSED(x)         ((void)(x))

/* ---- Port I/O ---------------------------------------------------------- */

static inline void outb(uint16_t port, uint8_t v)  { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(port)); }
static inline void outw(uint16_t port, uint16_t v) { __asm__ volatile("outw %0, %1" :: "a"(v), "Nd"(port)); }
static inline void outl(uint16_t port, uint32_t v) { __asm__ volatile("outl %0, %1" :: "a"(v), "Nd"(port)); }
static inline uint8_t  inb(uint16_t port) { uint8_t v;  __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline uint16_t inw(uint16_t port) { uint16_t v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline uint32_t inl(uint16_t port) { uint32_t v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void io_wait(void) { outb(0x80, 0); }

/* ---- CPU helpers ------------------------------------------------------- */

static inline void cpu_cli(void)   { __asm__ volatile("cli" ::: "memory"); }
static inline void cpu_sti(void)   { __asm__ volatile("sti" ::: "memory"); }
static inline void cpu_hlt(void)   { __asm__ volatile("hlt" ::: "memory"); }
static inline void cpu_pause(void) { __asm__ volatile("pause"); }

static inline uint64_t read_cr0(void) { uint64_t v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v; }
static inline uint64_t read_cr2(void) { uint64_t v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v; }
static inline uint64_t read_cr3(void) { uint64_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static inline uint64_t read_cr4(void) { uint64_t v; __asm__ volatile("mov %%cr4, %0" : "=r"(v)); return v; }
static inline uint64_t read_rflags(void) { uint64_t v; __asm__ volatile("pushfq; pop %0" : "=r"(v)); return v; }

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline void cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

NORETURN static inline void cpu_halt_forever(void)
{
    for (;;) { cpu_cli(); cpu_hlt(); }
}
