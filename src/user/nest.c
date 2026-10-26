/* WindClone — /bin/nest: the child that stays after its parent is gone.
 *
 * /bin/orphan exits without waiting, which makes this process the thing a
 * process table is only tested against once somebody dies too early: an orphan.
 * The kernel's own log line can say it re-parented this pid; it cannot say what
 * that *felt like* from inside the program that was adopted. So nest reports the
 * only number that changes — getppid(), the parent the table owns right now —
 * once per round, and lets the reader put the two halves together.
 *
 * What it deliberately does NOT do is decide anything. Reporting the parent each
 * round and exiting with a fixed 7 keeps the judgement where it belongs: which
 * round the adoption landed in is scheduling luck (a tick may switch to the
 * orphan at any point), so a program that only *observes* can be checked
 * order-free, while a program that asserted "I was adopted at round 3" would
 * make the test suite fragile for no extra truth. The exit code, on the other
 * hand, is the whole point of the release: before adoption, 7 died in a slot
 * whose parent no longer existed, and the sweep freed the memory without ever
 * telling anybody. Now the adopter reaps it and prints it.
 *
 * Six rounds, one yield apart: enough passes that the parent's exit necessarily
 * lands between two of this program's own lines, and few enough that the prompt
 * is not held up.
 *
 * Lines are composed and written once each, the way every scheduler demo has to:
 * a tick may preempt between two syscalls but never inside one.
 *
 * SPDX-License-Identifier: MIT
 */
#include "wcstr.h"

#define NEST_ROUNDS 6

/* What this program exits with. Nothing here computes it: the point is that a
 * *different* process than the one that made it is the one that collects it. */
#define NEST_EXIT 7

void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    long me = wc_getpid();

    for (int i = 1; i <= NEST_ROUNDS; i++) {
        long above = wc_getppid();

        char line[64];
        unsigned n = 0;
        n = wc_app_str(line, n, "[nest] round ");
        n = wc_app_dec(line, n, (unsigned long)i);
        n = wc_app_str(line, n, " pid ");
        n = wc_app_signed(line, n, me);
        n = wc_app_str(line, n, " ppid ");
        n = wc_app_signed(line, n, above);
        n = wc_app_str(line, n, "\n");
        wc_write(STDOUT_FILENO, line, n);

        wc_sched_yield();
    }

    char line[64];
    unsigned n = 0;
    n = wc_app_str(line, n, "[nest] exiting pid ");
    n = wc_app_signed(line, n, me);
    n = wc_app_str(line, n, " code ");
    n = wc_app_dec(line, n, NEST_EXIT);
    n = wc_app_str(line, n, "\n");
    wc_write(STDOUT_FILENO, line, n);

    wc_exit(NEST_EXIT);
}
