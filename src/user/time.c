/* WindClone — /bin/time: cmd's clock question, answered by the same chip.
 *
 * v0.1.48 gave `date` the fifteenth syscall; this is its twin. cmd keeps them
 * as two builtins that share a screen and a shape — print what the clock says,
 * offer to change it — and this program copies that shape rather than inventing
 * a tidier one.
 *
 * What it does NOT print: cmd's fractional hundredths. A real cmd answers
 * `The current time is: 23:17:18.57` because Windows keeps time at a much finer
 * grain than the CMOS chip. This clock is the chip: rtc.c reads it, checks that
 * the read settled, and hands out whole seconds. Two digits of invented
 * precision after a decimal point would say "I have a timer I do not have", so
 * the field is absent rather than padded with a fabricated .00. Everything else
 * about the line is cmd's.
 *
 * Why setting the time stops here: writing the chip means ports 0x70/0x71, and
 * ring 3 has no port privileges — the border is the whole point of the kernel.
 * cmd would set the clock; this echoes the input back and refuses with exit 1,
 * because a refusal that silently drops what you typed is indistinguishable
 * from acceptance. Which is worse than Windows, and says so in the message.
 *
 * The formats are cmd's own en-US answers, printed raw because CI greps these
 * lines and Windows users read them: `time /t` gives the short form (HH:MM) and
 * exits, bare `time` gives the long form and then the prompt.
 *
 * SPDX-License-Identifier: MIT
 */
#include "wcstr.h"

#define TM_LINE_MAX   64
#define TM_IN_MAX     32

#define TM_EXIT_OK        0
#define TM_EXIT_REFUSED   1   /* a time to set, or no clock behind the question */

static char line[TM_LINE_MAX];

static void emit_line(void)
{
    unsigned n = wc_strlen(line);

    line[n] = '\n';
    wc_write(STDOUT_FILENO, line, n + 1);
}

/* One whole line from a compile-time literal, one syscall, no 56-byte
 * appender cap in the way — date.c learned that lesson the hard way when a
 * 57-byte refusal arrived missing its closing parenthesis. `line` is 64, so a
 * literal through this path must stay under 63 chars, which is this file's own
 * fact to keep and the reason the longest literal here is 57. */
static void emit_str(const char *s)
{
    unsigned n = 0;

    while (s[n] && n < TM_LINE_MAX - 2) {
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

/* HH:MM, the short form `time /t` answers with. 24-hour because that is what
 * the chip's own register holds and what rtc.c decoded it into; no AM/PM is
 * invented here any more than a century was invented for the date. */
static void print_short(const struct wc_rtc *r)
{
    unsigned n = 0;

    app_pad2(line, &n, r->hour);
    line[n++] = ':';
    app_pad2(line, &n, r->minute);
    line[n] = '\0';
    emit_line();
}

/* The long form, cmd's "The current time is: " line — minus the fractional
 * digits this clock does not have. */
static void print_long(const struct wc_rtc *r)
{
    unsigned n = 0;

    n = wc_app_str(line, n, "The current time is: ");
    app_pad2(line, &n, r->hour);
    line[n++] = ':';
    app_pad2(line, &n, r->minute);
    line[n++] = ':';
    app_pad2(line, &n, r->second);
    line[n] = '\0';
    emit_line();
}

void _start(int argc, char **argv)
{
    struct wc_rtc r;
    long rc;

    /* cmd's shapes: `/t` prints short and stops; any other `/x` is an invalid
     * parameter, answered before the clock is even asked. */
    if (argc > 1 && argv[1][0] == '/') {
        if (!ci_eq(argv[1], "/t")) {
            unsigned n = wc_app_str(line, 0, "Invalid parameter - ");
            n = wc_app_str(line, n, argv[1] + 1);
            line[n] = '\0';
            emit_line();
            wc_exit(TM_EXIT_REFUSED);
        }
    }

    rc = wc_rtc(&r);
    if (rc != 0) {
        emit_str("time: no hardware clock answering");
        wc_exit(TM_EXIT_REFUSED);
    }

    if (argc > 1 && ci_eq(argv[1], "/t")) {
        print_short(&r);              /* cmd prints short and does not prompt */
        wc_exit(TM_EXIT_OK);
    }

    print_long(&r);
    if (argc > 1) {
        /* A time was handed over: cmd would set the clock. Writing CMOS is
         * ring-0 work, so this is where the copy stops and says so. */
        emit_str("time: setting the time is not supported (read-only clock)");
        wc_exit(TM_EXIT_REFUSED);
    }

    /* The interactive prompt, like cmd's: an empty Enter changes nothing and
     * that is success; anything else is named back and refused. */
    {
        unsigned n = wc_app_str(line, 0, "Enter the new time: ");
        line[n] = '\0';
        emit_line();
    }

    {
        char inbuf[TM_IN_MAX];
        unsigned len = 0;
        int saw_nl = 0;

        while (!saw_nl && len < TM_IN_MAX - 1) {
            char chunk[4];
            long got = wc_read(STDIN_FILENO, chunk, sizeof chunk);

            if (got <= 0)
                break;                /* error, or the keyboard handed back */
            for (long i = 0; i < got; i++) {
                if (chunk[i] == '\n' || chunk[i] == '\r') {
                    saw_nl = 1;
                    break;
                }
                inbuf[len++] = chunk[i];
            }
        }
        if (!saw_nl) {
            emit_str("time: setting the time is not supported (read-only clock)");
            wc_exit(TM_EXIT_REFUSED);
        }
        if (len == 0)
            wc_exit(TM_EXIT_OK);          /* empty Enter: unchanged, cmd's 0 */
        {
            /* cmd echoes what it could not parse; this echoes what it will not
             * set. The appenders clip at 56 bytes, so a long line of keyboard
             * cannot overrun `line` — the message just ends short, which is a
             * clipped report, not a crash. */
            unsigned m = wc_app_str(line, 0, "time: refusing to set '");
            m = wc_app_bytes(line, m, inbuf, len);
            m = wc_app_str(line, m, "' (read-only clock)");
            line[m] = '\0';
            emit_line();
            wc_exit(TM_EXIT_REFUSED);
        }
    }
}
