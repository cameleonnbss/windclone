/* WindClone — PIT channel 0 programming + tick accounting.
 *
 * The 8254 chip counts down from a divisor at 1.193182 MHz; when it
 * reaches zero it raises IRQ0. We pick 100 Hz so the log gets a
 * heartbeat once per second without drowning in serial output.
 *
 * SPDX-License-Identifier: MIT
 */
#include "timer.h"
#include "idt.h"
#include "io.h"

#define PIT_CHANNEL0 0x40
#define PIT_COMMAND  0x43

static volatile uint64_t ticks;

void timer_init(uint64_t hz)
{
    uint64_t divisor = PIT_HZ / hz;
    if (divisor == 0 || divisor > 0xFFFF)
        divisor = 0xFFFF;

    ticks = 0;

    /* 0x36 = channel 0 | access lo-then-hi | mode 3 (square wave) */
    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));

    pic_unmask(0);   /* IRQ0 = timer */
}

void timer_tick(void)
{
    ticks++;
}

uint64_t timer_ticks(void)
{
    return ticks;
}
