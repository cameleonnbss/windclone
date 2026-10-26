/* WindClone — COM1 serial driver implementation.
 * SPDX-License-Identifier: MIT
 */
#include "serial.h"
#include "io.h"

/* --- 16550 UART registers ------------------------------------------ */

#define COM1 0x3F8

#define DATA       (COM1 + 0)
#define IER        (COM1 + 1)  /* interrupt enable           */
#define FCR        (COM1 + 2)  /* FIFO control               */
#define LCR        (COM1 + 3)  /* line control               */
#define MSR        (COM1 + 4)  /* modem status               */
#define LSR        (COM1 + 5)  /* line status                */
#define LSR_READY  0x20        /* transmitter holding empty  */

void serial_init(void)
{
    outb(IER, 0x00);                    /* no interrupts yet       */
    outb(LCR, 0x80);                    /* enable DLAB             */
    outb(DATA, 0x03);                   /* divisor lo (38400 baud) */
    outb(IER, 0x00);                    /* divisor hi              */
    outb(LCR, 0x03);                    /* 8 bits, 1 stop, none    */
    outb(FCR, 0xC7);                    /* enable + clear FIFOs    */
    while ((inb(LSR) & 0x40) == 0)      /* wait for modem ready    */
        ;
}

void serial_putc(char c)
{
    while ((inb(LSR) & LSR_READY) == 0)
        ;
    outb(DATA, (uint8_t)c);
    if (c == '\n')
        serial_putc('\r');
}

void serial_write(const char *s)
{
    while (*s != '\0')
        serial_putc(*s++);
}

/* Byte-counted write: initrd files are *not* NUL-terminated (they are
 * slices of a tar archive), so they need the length, not a terminator. */
void serial_write_len(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        serial_putc(s[i]);
}

void serial_write_dec(uint64_t value)
{
    char buf[21];
    int i = 0;
    do {
        buf[i++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value != 0);
    while (i > 0)
        serial_putc(buf[--i]);
}

/* The signed form, because the interesting numbers in this kernel's log are
 * negative: an exit code of -SIGKILL, a -WC_EPERM, a -WC_ECHILD. Printing one
 * through the unsigned writer turns -9 into a twenty-digit number that was
 * never in the table at all. The magnitude is taken in unsigned so even
 * INT64_MIN, whose negation cannot be represented, comes out right. */
void serial_write_signed(int64_t value)
{
    if (value < 0) {
        serial_putc('-');
        serial_write_dec((uint64_t)0 - (uint64_t)value);
        return;
    }
    serial_write_dec((uint64_t)value);
}

static const char hex_digits[] = "0123456789abcdef";

void serial_write_hex(uint64_t value)
{
    serial_write("0x");
    /* print from the highest non-zero nibble */
    int started = 0;
    for (int shift = 60; shift >= 0; shift -= 4) {
        uint8_t nibble = (uint8_t)((value >> shift) & 0xF);
        if (!started && nibble == 0 && shift > 0)
            continue;
        started = 1;
        serial_putc(hex_digits[nibble]);
    }
}
