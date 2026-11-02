/* WindClone — /bin/date: cmd's clock question, answered by the CMOS chip.
 *
 * The taskbar tray has shown the time since the RTC driver landed, but the
 * tray shows; a program asks. cmd answers `date /t` from the hardware clock,
 * and this is that answer: the kernel's own decoded read, delivered through
 * the fifteenth syscall into a struct this program formats. No software clock
 * is kept here and nothing is invented: if no chip answered at boot, the
 * syscall says -WC_ENOENT and this program says there is no clock rather than
 * printing a plausible 01/01/1970 — a calendar prop is not a clock.
 *
 * Why the interactive form behaves like cmd's and then stops: `date` with no
 * arguments prints the current date, prompts for a new one, and an empty
 * Enter changes nothing and exits 0 — that whole loop is real cmd. Typing a
 * date is where the parity stops honest: setting the clock means writing
 * CMOS registers, and ring 3 has no port privileges (by design — the border
 * is the point), so a written date is refused with a stated reason and exit 1
 * rather than accepted and silently dropped. cmd would set it; this says no
 * out loud. Which is worse than Windows, and says so in the message.
 *
 * The century: the chip stores two digits and rtc.h refuses to guess the rest
 * of it. This program's declared rule is the one POSIX declared decades ago —
 * 00..69 is 20xx, 70..99 is 19xx — and the test pins that rule by arithmetic:
 * the weekday printed for the full four-digit year must be the weekday that
 * date actually had, so a wrong century is a wrong weekday and cannot hide.
 *
 * The format is cmd's own en-US answer, "The current date is: Thu 10/29/2026",
 * printed raw because CI greps these lines and Windows users read them.
 *
 * SPDX-License-Identifier: MIT
 */
#include "wcstr.h"

#define DT_LINE_MAX   64
#define DT_IN_MAX     32

#define DT_EXIT_OK        0
#define DT_EXIT_REFUSED   1   /* a date to set, or no clock behind the question */

static char line[DT_LINE_MAX];

static void emit_line(void)
{
    unsigned n = wc_strlen(line);

    line[n] = '\n';
    wc_write(STDOUT_FILENO, line, n + 1);
}

/* One whole line from a compile-time literal, one syscall, no 56-byte
 * appender cap in the way: the refusal below is 57 bytes and a message that
 * exists to be read cannot arrive missing its parenthesis. `line` is 64, so
 * a literal through this path must stay under 63 chars — a fact of this
 * file's own making, checked by the writer, not something argv can grow. */
static void emit_str(const char *s)
{
    unsigned n = 0;

    while (s[n] && n < DT_LINE_MAX - 2) {
        line[n] = s[n];
        n++;
    }
    line[n] = '\n';
    wc_write(STDOUT_FILENO, line, n + 1);
}

static char lc(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int ci_eq(const char *tok, const char *lit)
{
    for (;;) {
        if (lc(*tok) != *lit)
            return 0;
        if (*lit == '\0')
            return 1;
        tok++;
        lit++;
    }
}

static void app_pad2(char *out, unsigned *n, int v)
{
    out[(*n)++] = (char)('0' + v / 10);
    out[(*n)++] = (char)('0' + v % 10);
}

/* Sakamoto's day-of-week: returns 0 for Sunday .. 6 for Saturday. Integer
 * arithmetic only, which is the whole reason it is here and not something
 * with a strftime behind it. */
static int weekday(int y, int m, int d)
{
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };

    if (m < 3)
        y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

static void print_date(const struct wc_rtc *r)
{
    static const char *days[7] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };
    int year = r->year2 <= 69 ? 2000 + r->year2 : 1900 + r->year2;
    unsigned n = 0;

    n = wc_app_str(line, n, "The current date is: ");
    n = wc_app_str(line, n, days[weekday(year, r->month, r->day)]);
    line[n++] = ' ';
    app_pad2(line, &n, r->month);
    line[n++] = '/';
    app_pad2(line, &n, r->day);
    line[n++] = '/';
    n = wc_app_dec(line, n, (unsigned long)year);
    line[n] = '\0';
    emit_line();
}

void _start(int argc, char **argv)
{
    struct wc_rtc r;
    long rc;

    /* cmd's own shapes: `date /t` prints and stops; `date <mm-dd-yy>` sets;
     * a switch that is not /t is an invalid parameter. */
    if (argc > 1 && argv[1][0] == '/') {
        if (!ci_eq(argv[1], "/t")) {
            unsigned n = wc_app_str(line, 0, "Invalid parameter - ");
            n = wc_app_str(line, n, argv[1] + 1);
            line[n] = '\0';
            emit_line();
            wc_exit(DT_EXIT_REFUSED);
        }
    }

    rc = wc_rtc(&r);
    if (rc != 0) {
        unsigned n = wc_app_str(line, 0, "date: no hardware clock answering");
        line[n] = '\0';
        emit_line();
        wc_exit(DT_EXIT_REFUSED);
    }

    print_date(&r);
    if (argc > 1 && !ci_eq(argv[1], "/t")) {
        /* A date was handed over: cmd would set the clock. Writing CMOS is
         * ring-0 work, so this is where the copy stops and says so. */
        emit_str("date: setting the date is not supported (read-only clock)");
        wc_exit(DT_EXIT_REFUSED);
    }
    if (argc > 1)
        wc_exit(DT_EXIT_OK);        /* /t printed; nothing was asked of it */

    /* The interactive prompt, like cmd's: an empty Enter changes nothing and
     * that is success; anything else names the refusal. */
    {
        unsigned n = wc_app_str(line, 0, "Enter the new date (mm-dd-yy): ");
        line[n] = '\0';
        emit_line();
    }

    {
        char inbuf[DT_IN_MAX];
        unsigned len = 0;
        int saw_nl = 0;

        while (!saw_nl && len < DT_IN_MAX - 1) {
            char chunk[4];
            long got = wc_read(STDIN_FILENO, chunk, sizeof chunk);

            if (got < 0)
                break;                    /* read error: fall through refused */
            if (got == 0)
                break;                    /* the keyboard was handed back    */
            for (long i = 0; i < got; i++) {
                if (chunk[i] == '\n' || chunk[i] == '\r') {
                    saw_nl = 1;
                    break;
                }
                inbuf[len++] = chunk[i];
            }
        }
        if (!saw_nl) {
            emit_str("date: setting the date is not supported (read-only clock)");
            wc_exit(DT_EXIT_REFUSED);
        }
        if (len == 0)
            wc_exit(DT_EXIT_OK);          /* empty Enter: unchanged, cmd's 0 */
        {
            /* cmd echoes what it could not parse; this echoes what it will not
             * set, because a refusal that drops the input silently is the same
             * lie as accepting it. The appenders clip at 56 bytes, so a long
             * line of keyboard cannot overrun `line` — the message just ends
             * short, which is a clipped report, not a crash. */
            unsigned m = wc_app_str(line, 0, "date: refusing to set '");
            m = wc_app_bytes(line, m, inbuf, len);
            m = wc_app_str(line, m, "' (read-only clock)");
            line[m] = '\0';
            emit_line();
            wc_exit(DT_EXIT_REFUSED);
        }
    }
}
