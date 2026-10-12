#include "capmix/arch.h"
#include "capmix/console.h"
#include "capmix/klog.h"
#include "capmix/config.h"

#include <stdarg.h>
#include <stdint.h>

#ifdef CAPMIX_HOSTED
#include <stdio.h>
#include <time.h>
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
#endif

static cm_log_level_t g_level = CM_LOG_TRACE;

#ifdef CAPMIX_HOSTED
void cm_serial_init(void) {}
int  cm_klog_lock(void) { return 1; }
void cm_klog_unlock(void) {}
void cm_serial_putchar(char c) { fputc(c, stdout); }
uint64_t cm_rdtsc(void)
{
#if defined(__x86_64__)
    unsigned int lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#else
    return cm_ticks_now();
#endif
}
uint64_t cm_ticks_now(void)
{
    return cm_rdtsc();
}
void cm_arch_early_init(void)
{
    cm_serial_init();
    cm_gdt_load();
    cm_idt_load();
}
#else
/* Freestanding COM1. */
#define COM1 0x3F8
static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* SMP: one UART, N CPUs, plus preemption. Two locks, two granularities:
 *
 *   g_uart_lock      per CHARACTER - guards the single 16550 data register
 *   g_uart_msg_lock  per MESSAGE   - guards a whole klog line
 *
 * Character-level alone is NOT enough: cm_klog emits '[','I','N','F',']',' '
 * one putchar at a time, so another CPU lands between them and you get
 * "[INF] [ap: cpu=INF] 1 ...". The message lock keeps a LINE intact.
 *
 * Both spins are BOUNDED, and "bounded" now actually means something:
 * cm_fault_isr() prints and then halts FOREVER while holding the message
 * lock, so an unbounded wait would hang every other CPU with it. Worse,
 * the previous code released the lock even when the spin had given up —
 * i.e. it cleared a lock it did not own, which destroys mutual exclusion
 * for everyone. That is exactly why the log tore right after a [FAULT]:
 * the faulting CPU died holding the lock, the others timed out, "released"
 * it, and then all printed at once. */
static volatile unsigned int g_uart_lock;
static volatile unsigned int g_uart_msg_lock;
#define CM_UART_SPIN_MAX 20000000u

/* Old RFLAGS per CPU, saved by cm_klog_lock(). Holding the message lock
 * with interrupts off is what stops THIS cpu from being preempted mid-line:
 * otherwise the timer ISR can swap the holder out while it owns the lock,
 * and every other CPU waits a full time slice for a line that is only
 * half-written. */
static uint64_t g_uart_msg_flags[CM_MAX_CPUS];
static int      g_uart_msg_held[CM_MAX_CPUS];

/* Returns 1 on success, 0 if we gave up. NEVER release after a 0. */
static int uart_spin_acquire(volatile unsigned int *l)
{
    unsigned int n = 0;
    while (__sync_lock_test_and_set(l, 1u)) {
        if (++n > CM_UART_SPIN_MAX) return 0;
        __asm__ volatile("pause" ::: "memory");
    }
    return 1;
}

static void uart_spin_release(volatile unsigned int *l)
{
    __sync_lock_release(l);
}

int cm_klog_lock(void)
{
    int cpu = cm_cpu_id();

    /* Nested — we are a fault ISR that interrupted our own cm_klog, which
     * already holds the lock. Returning 0 makes the caller skip the line;
     * without this check we would overwrite g_uart_msg_flags[] with the
     * ISR's IF=0, and the outer unlock would then never re-enable
     * interrupts on this CPU. */
    if (g_uart_msg_held[cpu]) return 0;

    uint64_t f;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    if (!uart_spin_acquire(&g_uart_msg_lock)) {
        /* Give up, but restore IF exactly as we found it. */
        if (f & 0x200) __asm__ volatile("sti" ::: "memory");
        return 0;
    }
    g_uart_msg_flags[cpu] = f;
    g_uart_msg_held[cpu]  = 1;
    return 1;
}

void cm_klog_unlock(void)
{
    int cpu = cm_cpu_id();
    if (!g_uart_msg_held[cpu]) return;      /* nothing to release */
    uint64_t f = g_uart_msg_flags[cpu];
    g_uart_msg_held[cpu] = 0;
    uart_spin_release(&g_uart_msg_lock);
    if (f & 0x200) __asm__ volatile("sti" ::: "memory");
}

void cm_serial_init(void)
{
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

void cm_serial_putchar(char c)
{
    uint64_t f;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    if (!uart_spin_acquire(&g_uart_lock)) {
        if (f & 0x200) __asm__ volatile("sti" ::: "memory");
        return;                       /* drop the char, keep exclusion */
    }
    if (c == '\n') {
        while ((inb(COM1 + 5) & 0x20) == 0) {}
        outb(COM1, (uint8_t)'\r');
    }
    while ((inb(COM1 + 5) & 0x20) == 0) {}
    outb(COM1, (uint8_t)c);
    uart_spin_release(&g_uart_lock);
    if (f & 0x200)
        __asm__ volatile("sti" ::: "memory");
}

uint64_t cm_rdtsc(void)
{
    unsigned int lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
uint64_t cm_ticks_now(void) { return cm_rdtsc(); }
void cm_arch_early_init(void)
{
    cm_serial_init();
    cm_gdt_load();
    cm_idt_load();
}
#endif

void cm_klog_init(void) { g_level = CM_LOG_TRACE; }

void cm_putchar(char c) { cm_serial_putchar(c); cm_console_putchar(c); }

void cm_puts(const char *s)
{
    if (!s) return;
    while (*s) cm_putchar(*s++);
}

static void put_uint(uint64_t v, int base, int width)
{
    char buf[32];
    const char *dig = "0123456789abcdef";
    int i = 0;
    if (v == 0) buf[i++] = '0';
    while (v && i < (int)sizeof(buf)) {
        buf[i++] = dig[v % (uint64_t)base];
        v /= (uint64_t)base;
    }
    while (i < width) buf[i++] = '0';
    while (i--) cm_putchar(buf[i]);
}

static void kvprintf(const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') { cm_putchar(*fmt); continue; }
        fmt++;
        int longish = 0;
        while (*fmt == 'l') { longish++; fmt++; }
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            cm_puts(s ? s : "(null)");
            break;
        }
        case 'd': {
            int64_t n = (longish >= 1) ? va_arg(ap, long) : va_arg(ap, int);
            if (n < 0) { cm_putchar('-'); n = -n; }
            put_uint((uint64_t)n, 10, 0);
            break;
        }
        case 'u': {
            uint64_t n = (longish >= 1) ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
            put_uint(n, 10, 0);
            break;
        }
        case 'x': {
            uint64_t n = (longish >= 1) ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
            put_uint(n, 16, 0);
            break;
        }
        case 'p': {
            cm_puts("0x");
            put_uint((uint64_t)(uintptr_t)va_arg(ap, void *), 16, 16);
            break;
        }
        case 'c':
            cm_putchar((char)va_arg(ap, int));
            break;
        case '%':
            cm_putchar('%');
            break;
        default:
            cm_putchar('%');
            cm_putchar(*fmt);
            break;
        }
    }
}

void cm_klog(cm_log_level_t level, const char *fmt, ...)
{
    static const char *tag[] = { "TRC", "INF", "WRN", "ERR", "!!!" };
    if (level < g_level) return;
    /* Bounded wait: if we could not get the lock, drop the WHOLE line
     * rather than emit a torn one. Losing a message is survivable; a
     * half-interleaved log hides the very crash you are debugging. */
    if (!cm_klog_lock()) return;
    cm_putchar('[');
    cm_puts(tag[level]);
    cm_puts("] ");
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    cm_putchar('\n');
    cm_klog_unlock();
#ifdef CAPMIX_HOSTED
    fflush(stdout);
#endif
}

void cm_panic(const char *fmt, ...)
{
    int got = cm_klog_lock();
    cm_puts("\n*** PANIC: ");
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    cm_puts(" ***\n");
    /* Only unlock if we actually took it — and note that panicking while
     * someone else holds the lock is impossible to fully recover from, so
     * we deliberately do NOT unlock on failure. */
    if (got) cm_klog_unlock();
#ifdef CAPMIX_HOSTED
    fflush(stdout);
    __builtin_trap();
#else
    for (;;) { __asm__ volatile("hlt"); }
#endif
}