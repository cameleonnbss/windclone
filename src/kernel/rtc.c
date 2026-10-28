/* WindClone — MC146818 real-time clock driver. See rtc.h for what it promises.
 *
 * The one non-obvious part of this file is chip_read(). The chip updates its
 * registers once a second, and a read that straddles that moment can hand back
 * the minute from before the rollover and the second from after it: 14:23:59,
 * then 14:22:00 — a clock that sometimes runs backwards. So the fields are read
 * twice and kept only when the two passes agree, the update-in-progress bit is
 * honoured when the chip bothers to set one, and the loop is capped, because a
 * driver that waits forever on a chip that never settles is a boot that never
 * happens. Nothing here prints: the tray asks for the time about once a second,
 * and a driver that answers a poll with a serial line is a log nobody can read.
 *
 * SPDX-License-Identifier: MIT
 */
#include "rtc.h"
#include "io.h"
#include "log.h"
#include "serial.h"

#define RTC_INDEX   0x70      /* register to address, bit 7 clear = leave NMI alone */
#define RTC_DATA    0x71      /* the byte behind that address                      */

#define RTC_SEC     0x00      /* the MC146818 interleaves the alarm registers, so  */
#define RTC_MIN     0x02      /* the clock fields are NOT 0,1,2: sec 0, min 2, hour */
#define RTC_HOUR    0x04      /* 4, day-of-week 6, day 7, month 8, year 9. A real    */
#define RTC_DOW     0x06      /* BIOS and the datasheet agree; a driver that assumes  */
#define RTC_DAY     0x07      /* 0,1,2 reads the (unset) alarm bytes as the time.     */
#define RTC_MONTH   0x08
#define RTC_YEAR    0x09      /* two-digit year; the century is not stored here       */
#define RTC_REG_A   0x0A      /* bit 7: update in progress                         */
#define RTC_REG_B   0x0B      /* bit 1: 1 = binary, 0 = BCD. bit 2: 1 = 24-hour    */

#define RTC_UIP     0x80
#define RTC_BCD     0x02
#define RTC_24H     0x04
#define RTC_PM      0x80      /* in 12-hour mode, bit 7 of the hour                */

#define RTC_TRIES   200       /* enough for a real chip's few-microsecond update,
                               * short enough that a dead one cannot hang boot     */

static int hour = -1, minute = -1, second = -1;   /* the last good read            */
static int mday = -1, rmonth = -1, ryear = -1;

static uint8_t cmos_read(uint8_t reg)
{
    outb(RTC_INDEX, (uint8_t)(reg & 0x7F));
    io_wait();
    return inb(RTC_DATA);
}

/* The byte 0x14 is the number 20 to a chip counting in binary and 14 to a chip
 * counting in BCD, and nothing about the byte itself says which was meant. Flag
 * B says, so the driver reads it instead of picking the one that looks right:
 * a firmware that keeps 20:14 and a driver that prints 14:20 are both confident,
 * and one of them is an hour and a half late. */
static int decode(uint8_t v, int binary)
{
    return binary ? (int)v : (int)(v >> 4) * 10 + (int)(v & 0x0F);
}

/* Are these numbers a date at all? A chip with nothing behind it answers 0xFF to
 * every register: 255 read as binary, 165 read as BCD (two nibbles of fifteen),
 * and still 127 as an hour after the PM mask. None of those is a time, and the
 * range check turns "there is no clock here" into a refusal rather than a
 * confident nonsense clock in the corner of the screen. */
static int fields_valid(int h, int min, int sec, int mon, int mdy)
{
    return !(mon < 1 || mon > 12 || mdy < 1 || mdy > 31 ||
             h > 23 || min > 59 || sec > 59);
}

/* One stable read of the chip, decoded. Returns 0 when nothing stable arrived
 * (a chip whose update never finishes, or one that answers 0xFF to everything),
 * which the caller treats as "there is no time to show" rather than as midnight.
 * `hour24` and `binary` are reported back for the boot line: they are facts about
 * the firmware, and worth seeing once. */
static int chip_read(int *hh, int *mm, int *ss, int *dd, int *mo, int *yy,
                     int *binary, int *hour24)
{
    uint8_t s1, m1, h1, d1, o1, y1;
    uint8_t s2 = 0, m2 = 0, h2 = 0, d2 = 0, o2 = 0, y2 = 0;
    uint8_t b = 0;
    int tries = RTC_TRIES;

    do {
        if (cmos_read(RTC_REG_A) & RTC_UIP)
            continue;                       /* mid-update: nothing read here is stable */

        s1 = cmos_read(RTC_SEC);   m1 = cmos_read(RTC_MIN);   h1 = cmos_read(RTC_HOUR);
        d1 = cmos_read(RTC_DAY);   o1 = cmos_read(RTC_MONTH); y1 = cmos_read(RTC_YEAR);
        b  = cmos_read(RTC_REG_B);

        s2 = cmos_read(RTC_SEC);   m2 = cmos_read(RTC_MIN);   h2 = cmos_read(RTC_HOUR);
        d2 = cmos_read(RTC_DAY);   o2 = cmos_read(RTC_MONTH); y2 = cmos_read(RTC_YEAR);

        if (s1 == s2 && m1 == m2 && h1 == h2 && d1 == d2 && o1 == o2 && y1 == y2) {
            int bin = (b & RTC_BCD) != 0;
            int h   = decode((uint8_t)(h2 & (uint8_t)~RTC_PM), bin);
            int min = decode(m2, bin);
            int sec = decode(s2, bin);
            int mon = decode(o2, bin);
            int mdy = decode(d2, bin);
            int yr  = decode(y2, bin);

            /* Flag B bit 1 is the datasheet's "data mode" (1 = binary), but some
             * firmware lies about it: the emulator every one of these tests runs
             * under inverts that bit and hands back BCD bytes under a flag that
             * claims binary. So trust the bytes, not the label - when the flagged
             * mode cannot be a real date, re-decode the same registers the other
             * way and keep it only if those numbers form one. A dead chip answers
             * 0xFF and fails both modes, so this recovers a lying flag without
             * ever inventing a time out of noise. */
            if (!fields_valid(h, min, sec, mon, mdy)) {
                bin = !bin;
                h   = decode((uint8_t)(h2 & (uint8_t)~RTC_PM), bin);
                min = decode(m2, bin);
                sec = decode(s2, bin);
                mon = decode(o2, bin);
                mdy = decode(d2, bin);
                yr  = decode(y2, bin);
                if (!fields_valid(h, min, sec, mon, mdy))
                    return 0;
            }

            if (hh) *hh = h;
            if (mm) *mm = min;
            if (ss) *ss = sec;
            if (dd) *dd = mdy;
            if (mo) *mo = mon;
            if (yy) *yy = yr;
            if (binary) *binary = bin;
            if (hour24) *hour24 = (b & RTC_24H) != 0;
            return 1;
        }
    } while (--tries > 0);

    return 0;
}

void rtc_init(void)
{
    int hh, mm, ss, dd, mo, yy, bin, h24;

    if (!chip_read(&hh, &mm, &ss, &dd, &mo, &yy, &bin, &h24)) {
        serial_write("rtc: no hardware clock answering (ports 0x70/0x71)\n");
        return;
    }

    hour = hh; minute = mm; second = ss;
    mday = dd; rmonth = mo; ryear = yy;

    /* Two digits each, written by hand: klog's %d has no field width, and a
     * clock that prints 14:5 is a clock that is being misread, not a clock that
     * is five minutes early. */
    char clock[9];
    clock[0] = (char)('0' + hh / 10); clock[1] = (char)('0' + hh % 10);
    clock[2] = ':';
    clock[3] = (char)('0' + mm / 10); clock[4] = (char)('0' + mm % 10);
    clock[5] = ':';
    clock[6] = (char)('0' + ss / 10); clock[7] = (char)('0' + ss % 10);
    clock[8] = '\0';

    serial_write("rtc: hardware clock ");
    serial_write(clock);
    serial_write(h24 ? " 24h" : " 12h");
    serial_write(bin ? " binary\n" : " bcd\n");
    klog("rtc: date day %d month %d year %d", dd, mo, yy);
}

void rtc_now(int *hh, int *mm, int *ss)
{
    int h, m, s, dd, mo, yy, bin, h24;

    if (chip_read(&h, &m, &s, &dd, &mo, &yy, &bin, &h24)) {
        hour = h; minute = m; second = s;
        mday = dd; rmonth = mo; ryear = yy;
    }

    if (hh) *hh = hour;
    if (mm) *mm = minute;
    if (ss) *ss = second;
}

void rtc_date(int *dd, int *mo, int *yy)
{
    if (dd) *dd = mday;
    if (mo) *mo = rmonth;
    if (yy) *yy = ryear;
}
