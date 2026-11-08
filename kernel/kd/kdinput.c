/*
 * Kd - keyboard input for the kernel debug shell.
 *
 * Characters come from COM1 (receive interrupt) and from a PS/2 keyboard
 * (scan code set 1, US layout). Both interrupt handlers put characters in a
 * ring buffer and queue a DPC, and the DPC sets an event the shell waits on.
 * Phase 1.5 replaces this with the real keyboard and TTY drivers.
 */
#include "ke.h"
#include "hal.h"
#include "rtl.h"
#include "console.h"

#define RING 256
#define COM1 0x3F8

static char g_ring[RING];
static volatile unsigned g_head, g_tail;
static KSPIN_LOCK g_lock;
static KEVENT g_event;
static KDPC g_dpc;
static bool g_shift, g_ctrl, g_caps, g_e0;
static bool g_have_ps2;

static void push(char c)
{
    KIRQL old = KeAcquireSpinLockRaiseToHigh(&g_lock);
    unsigned next = (g_head + 1) % RING;
    if (next != g_tail) {
        g_ring[g_head] = c;
        g_head = next;
    }
    KeReleaseSpinLock(&g_lock, old);
}

static void push_str(const char *s)
{
    while (*s)
        push(*s++);
}

static void input_dpc(KDPC *dpc, void *ctx, void *a1, void *a2)
{
    UNUSED(dpc); UNUSED(ctx); UNUSED(a1); UNUSED(a2);
    KeSetEvent(&g_event);
}

static void serial_isr(KTRAP_FRAME *f, void *ctx)
{
    UNUSED(f); UNUSED(ctx);
    while (inb(COM1 + 5) & 0x01)
        push((char)inb(COM1));
    KeInsertQueueDpc(&g_dpc, NULL, NULL);
}

/* Scan code set 1 -> ASCII (US). */
static const char g_normal[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\r', 0, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', 0, 0, 0, 0, 0,
};
static const char g_shifted[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\r', 0, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', 0, 0, 0, 0, 0,
};

static void keyboard_scancode(uint8_t sc)
{
    if (sc == 0xE0) {
        g_e0 = true;
        return;
    }
    bool release = sc & 0x80;
    uint8_t code = sc & 0x7F;
    bool e0 = g_e0;
    g_e0 = false;

    if (code == 0x2A || code == 0x36) { g_shift = !release; return; }
    if (code == 0x1D) { g_ctrl = !release; return; }
    if (release)
        return;
    if (code == 0x3A) { g_caps = !g_caps; return; }
    if (e0) {
        switch (code) {
        case 0x48: push_str("\x1b[A"); return;
        case 0x50: push_str("\x1b[B"); return;
        case 0x4D: push_str("\x1b[C"); return;
        case 0x4B: push_str("\x1b[D"); return;
        case 0x1C: push('\r'); return;           /* keypad Enter */
        case 0x35: push('/'); return;
        default: return;
        }
    }
    char c = g_shift ? g_shifted[code] : g_normal[code];
    if (g_caps && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
        c ^= 0x20;
    if (g_ctrl && c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 1);
    else if (g_ctrl && c >= 'A' && c <= 'Z')
        c = (char)(c - 'A' + 1);
    if (c)
        push(c);
}

static void keyboard_isr(KTRAP_FRAME *f, void *ctx)
{
    UNUSED(f); UNUSED(ctx);
    for (int i = 0; i < 16 && (inb(0x64) & 0x01); i++) {
        uint8_t status = inb(0x64);
        uint8_t data = inb(0x60);
        if (!(status & 0x20))            /* not from the mouse port */
            keyboard_scancode(data);
    }
    KeInsertQueueDpc(&g_dpc, NULL, NULL);
}

void KdInitializeInput(void)
{
    KeInitializeEvent(&g_event, SynchronizationEvent, false);
    KeInitializeDpc(&g_dpc, input_dpc, NULL);

    if (HalSerialPresent()) {
        uint8_t v = KeAllocateVectorAtIrql(4);
        KeConnectInterrupt(v, serial_isr, NULL);
        while (inb(COM1 + 5) & 0x01)
            (void)inb(COM1);
        outb(COM1 + 1, 0x01);            /* interrupt on received data */
        HalEnableIsaIrq(4, v);
    }

    if (inb(0x64) != 0xFF) {
        g_have_ps2 = true;
        for (int i = 0; i < 32 && (inb(0x64) & 0x01); i++)
            (void)inb(0x60);             /* drop anything the firmware left */
        uint8_t v = KeAllocateVectorAtIrql(5);
        KeConnectInterrupt(v, keyboard_isr, NULL);
        HalEnableIsaIrq(1, v);
    }
    DbgPrint("Kd: debug shell input on %s%s%s", HalSerialPresent() ? "COM1" : "",
             HalSerialPresent() && g_have_ps2 ? " and " : "", g_have_ps2 ? "PS/2 keyboard" : "");
}

/* Returns the next character, or -1 on timeout. */
int KdGetChar(int64_t timeout_ns)
{
    for (;;) {
        KIRQL old = KeAcquireSpinLockRaiseToHigh(&g_lock);
        if (g_tail != g_head) {
            char c = g_ring[g_tail];
            g_tail = (g_tail + 1) % RING;
            KeReleaseSpinLock(&g_lock, old);
            return (unsigned char)c;
        }
        KeReleaseSpinLock(&g_lock, old);
        if (KeWaitForSingleObject(&g_event, timeout_ns) == STATUS_TIMEOUT)
            return -1;
    }
}
