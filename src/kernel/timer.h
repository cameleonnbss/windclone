/* WindClone — PIT (8254) timer and kernel tick counter.
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_TIMER_H
#define WINDCLONE_TIMER_H

#include <stdint.h>

#define PIT_HZ 1193182ull   /* fixed oscillator feeding the 8254 */

void     timer_init(uint64_t hz);   /* PIT channel 0 -> IRQ0 at `hz` */
uint64_t timer_ticks(void);         /* ticks since timer_init()      */
void     timer_tick(void);          /* called from irq_handler       */

#endif
