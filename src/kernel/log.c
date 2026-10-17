/* WindClone — boot messages ring buffer (Phase 3).
 *
 * Why a ring and not just the serial port: serial output vanishes the
 * moment QEMU/CI closes the log file, and on real hardware nobody is
 * plugged in. Keeping the last N formatted messages in RAM means the
 * Phase 5 debug shell can show "what the kernel said during boot" even
 * after the fact. Drivers call klog() (echo + record) or log_message()
 * (record only, for noisy per-IRQ lines).
 *
 * The formatter is a deliberately tiny subset of printf: %c %s %d %u
 * %x with optional 0-width (%04x). No floats, no malloc — this must be
 * safe to call from interrupt context, which it is on our single CPU
 * (one writer at a time; the shell only reads between interrupts).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>

#include "log.h"
#include "serial.h"

#define LOG_MAX  48   /* messages kept */
#define LOG_LINE 128  /* bytes per message */

static char ring[LOG_MAX][LOG_LINE];
static int head;     /* next slot to write */
static int filled;   /* valid entries (capped at LOG_MAX) */

void log_init(void)
{
    head = 0;
    filled = 0;
    serial_write("step: log ring armed (");
    serial_write_dec(LOG_MAX);
    serial_write(" x ");
    serial_write_dec(LOG_LINE);
    serial_write(" bytes)\n");
}

/* --- tiny scalar formatters (no SSE anywhere, per kernel policy) --- */

static int enc_dec(char *out, int cap, long long v)
{
    char tmp[24];
    int t = 0, n = 0;
    unsigned long long u;
    if (v < 0) {
        if (cap > 0) out[n++] = '-';
        u = (unsigned long long)-(v + 1) + 1ULL; /* safe for LLONG_MIN */
    } else {
        u = (unsigned long long)v;
    }
    do {
        tmp[t++] = (char)('0' + (u % 10));
        u /= 10;
    } while (u && t < 20);
    while (t > 0 && n < cap)
        out[n++] = tmp[--t];
    return n;
}

static int enc_hex(char *out, int cap, unsigned long long v, int width)
{
    static const char dig[] = "0123456789abcdef";
    char tmp[17];
    int t = 0, n = 0;
    do {
        tmp[t++] = dig[v & 0xF];
        v >>= 4;
    } while (v && t < 16);
    while (t < width && t < 16)
        tmp[t++] = '0';
    while (t > 0 && n < cap)
        out[n++] = tmp[--t];
    return n;
}

static int format_line(char *dst, int cap, const char *fmt, va_list ap)
{
    int n = 0;
    while (*fmt && n < cap - 1) {
        if (*fmt != '%') {
            dst[n++] = *fmt++;
            continue;
        }
        fmt++;
        if (*fmt == '%') {
            dst[n++] = '%';
            fmt++;
            continue;
        }
        int width = 0;
        if (*fmt == '0') {
            fmt++;
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + (*fmt++ - '0');
        }
        switch (*fmt) {
        case 'c':
            dst[n++] = (char)va_arg(ap, int);
            fmt++;
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (s == NULL) s = "(null)";
            while (*s && n < cap - 1)
                dst[n++] = *s++;
            fmt++;
            break;
        }
        case 'd':
            n += enc_dec(dst + n, cap - 1 - n, (long long)va_arg(ap, int));
            fmt++;
            break;
        case 'u':
            n += enc_dec(dst + n, cap - 1 - n,
                         (long long)va_arg(ap, unsigned int));
            fmt++;
            break;
        case 'x':
            n += enc_hex(dst + n, cap - 1 - n,
                         (unsigned long long)va_arg(ap, unsigned int),
                         width);
            fmt++;
            break;
        default:
            /* Unknown spec: emit it literally rather than corrupt the
             * rest of the message. */
            dst[n++] = '%';
            break;
        }
    }
    dst[n] = '\0';
    return n;
}

static void ring_add(const char *msg)
{
    int i = 0;
    for (; i < LOG_LINE - 1 && msg[i] != '\0'; i++)
        ring[head][i] = msg[i];
    ring[head][i] = '\0';
    head = (head + 1) % LOG_MAX;
    if (filled < LOG_MAX)
        filled++;
}

void klog(const char *fmt, ...)
{
    char msg[LOG_LINE];
    va_list ap;
    va_start(ap, fmt);
    format_line(msg, (int)sizeof(msg), fmt, ap);
    va_end(ap);
    ring_add(msg);
    serial_write(msg);
    serial_write("\n");
}

void log_message(const char *fmt, ...)
{
    char msg[LOG_LINE];
    va_list ap;
    va_start(ap, fmt);
    format_line(msg, (int)sizeof(msg), fmt, ap);
    va_end(ap);
    ring_add(msg);
}

/* Most recent n messages, oldest first, " | " separated. */
void log_dump_tail(int n)
{
    if (n > filled)
        n = filled;
    for (int i = n - 1; i >= 0; i--) {
        int idx = (head - 1 - i + 2 * LOG_MAX) % LOG_MAX;
        serial_write(ring[idx]);
        if (i > 0)
            serial_write(" | ");
    }
}

int log_entries(void)
{
    return filled;
}
