/* WindClone — COM1 serial driver implementation.
 * SPDX-License-Identifier: MIT
 */
#include "serial.h"

/* --- minimal port I/O (x86 in/out instructions) -------------------- */

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

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
