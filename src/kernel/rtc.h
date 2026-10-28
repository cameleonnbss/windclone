/* WindClone — MC146818 real-time clock (the CMOS chip behind ports 0x70/0x71).
 *
 * Seconds, minutes, hours, day, month and year live in registers 0x00..0x09 and
 * the chip answers through an index port and a data port: one read is two port
 * operations and a decode. No interrupt line is needed and none is used — the
 * desktop asks for the time when it paints the tray, and the taskbar clock has
 * never needed better than a minute.
 *
 * The chip counts in BCD or in binary and keeps 12- or 24-hour time, and which
 * of those it is doing is written in its own flag register rather than assumed
 * by the driver, because a firmware that says 14:53 and a driver that prints
 * 20:53 are both confident and one of them is wrong.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_RTC_H
#define WINDCLONE_RTC_H

/* Read the chip once, work out how it counts, and put the answer on the serial
 * line. Safe before the framebuffer: ports only, no allocation, no interrupt. */
void rtc_init(void);

/* The clock, 24-hour, decoded. Each value is -1 when there was no chip
 * answering (or one whose update never finished), which the tray reads as
 * "draw nothing" rather than as midnight. */
void rtc_now(int *hh, int *mm, int *ss);

/* Day, month and a two-digit year. No century: not every firmware stores one,
 * and inventing a "20" in front of a bare "26" is a guess wearing the face of a
 * fact. */
void rtc_date(int *dd, int *mo, int *yy);

#endif
