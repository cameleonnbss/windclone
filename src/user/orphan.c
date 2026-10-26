/* WindClone — /bin/orphan: the parent that leaves first.
 *
 * Every program the process table had run so far was a parent that *stayed*:
 * /bin/waiter slept until a child died, /bin/reaper decided when, and both
 * collected what they were owed. What nobody had shown was the other direction
 * — a parent that dies while its child is still running — and that is the case
 * where a process table either has an answer or quietly loses one. Without
 * adoption the child keeps a dead process's pid in its `parent` field forever:
 * no wait() can ever name it, and the exit code it eventually returns is
 * written into a slot nobody is left to read.
 *
 * So this program does the smallest dishonest-feeling thing a parent can do:
 *
 *   1. spawn /bin/nest, which runs on for several rounds after this one ends;
 *   2. print its own pid, its own parent and the child it just made, so the
 *      three numbers the kernel is about to reason about are on the log from
 *      ring 3 as well as from ring 0;
 *   3. exit 0 immediately — no wait(), no kill, no apology.
 *
 * Refusing to wait is the whole test. A parent that reaps its own children never
 * exercises the adoption path, and the difference between "the sweep freed the
 * leftover" and "the adopter collected an exit code" is invisible until somebody
 * leaves early. /bin/nest is the one who stays.
 *
 * Lines are composed and written once each, the way every scheduler demo has to:
 * a tick may preempt between two syscalls but never inside one.
 *
 * SPDX-License-Identifier: MIT
 */
#include "wcstr.h"

void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    long me = wc_getpid();
    long above = wc_getppid();

    char *kargv[1] = { "nest" };
    long kid = wc_spawn_args("/bin/nest", 1, kargv);

    char line[64];
    unsigned n;

    n = 0;
    n = wc_app_str(line, n, "[orphan] pid ");
    n = wc_app_signed(line, n, me);
    n = wc_app_str(line, n, " ppid ");
    n = wc_app_signed(line, n, above);
    n = wc_app_str(line, n, " child ");
    n = wc_app_signed(line, n, kid);
    n = wc_app_str(line, n, "\n");
    wc_write(STDOUT_FILENO, line, n);

    /* Said out loud, because the absence of a wait() is the claim: everything
     * after this line belongs to a process that has no parent left to ask. */
    wc_puts("[orphan] leaving now, I will not wait\n");

    wc_exit(0);
}
