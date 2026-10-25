/* WindClone — /bin/bell: a process that exists to be killed.
 *
 * The kill() demo needs a victim that is *alive and busy* when it is stopped —
 * not one that exits on its own, because a kill that arrives after a natural
 * exit proves nothing. So this program works through its rounds with no yields
 * and no exit of its own until the very last one: it is a live, compute-bound
 * process the whole time it is running, which is the only shape that makes
 * "the parent called kill() and this stopped" a real observation.
 *
 * The evidence is in two parts, and the second is an absence:
 *
 *     [bell] round 1        <- it was running, printing as it went
 *     [bell] round 2
 *     (no [bell] done)      <- and then it never reached the end
 *
 * Rounds 1..5 print; the rest burn silently, because the log is a serial port
 * and a killed process should not have written 400 lines to prove one point.
 * A round is short compared with the 10 ms the timer gives this process before
 * the scheduler takes it back, so the first lines are on the log well before
 * the parent reaches its kill(). The whole run is far longer than that, and
 * deliberately not infinite: if kill() were broken, this finishes on its own,
 * the parent's wait() returns code 0 instead of -9, and CI says so instead of
 * hanging.
 *
 * There is no label in these lines, unlike /bin/spin: exactly one bell runs,
 * and its pid is what the parent's kill() and wait() cross-check against.
 *
 * SPDX-License-Identifier: MIT
 */
#include "wcstr.h"

#define BELL_ROUNDS 400
#define BELL_WORK   2000000ULL
#define BELL_NOISE  5

void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    long pid = wc_getpid();

    {
        char line[64];
        unsigned n = 0;
        n = wc_app_str(line, n, "[bell] ringing pid ");
        n = wc_app_dec(line, n, (unsigned long)(pid < 0 ? 0 : pid));
        n = wc_app_str(line, n, "\n");
        wc_write(STDOUT_FILENO, line, n);
    }

    volatile unsigned long burn = 0;
    for (int r = 1; r <= BELL_ROUNDS; r++) {
        for (unsigned long i = 0; i < BELL_WORK; i++)
            burn += i;
        if (r > BELL_NOISE)
            continue;

        char line[64];
        unsigned n = 0;
        n = wc_app_str(line, n, "[bell] round ");
        n = wc_app_dec(line, n, (unsigned long)r);
        n = wc_app_str(line, n, "\n");
        wc_write(STDOUT_FILENO, line, n);
    }

    /* Should never be reached while the parent is doing its job. */
    wc_puts("[bell] done\n");
    wc_exit(0);
}
