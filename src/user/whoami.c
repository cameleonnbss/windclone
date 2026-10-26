/* WindClone — /bin/whoami: ask the kernel what you are, and what you cannot end.
 *
 * Two questions, both about the process table, both answerable only from the
 * kernel side of the boundary:
 *
 *   1. `getpid()` — until the shell had a slot of its own, this answered 0 for
 *      every program typed at the prompt or clicked in the start menu, because
 *      usermode_run() has no process table: the program was an excursion into
 *      ring 3, not a process. It now answers the pid `proc: spawned ...` put in
 *      the log a moment earlier, and CI compares the two. That cross-check is
 *      the whole proof of this release: a number the kernel handed out at
 *      spawn time, read back by the program that received it.
 *   2. `kill(1, SIGKILL)` — pid 1 is the shell. It is in the table, so the
 *      answer is not `-WC_ESRCH`; it has no address space, no trap frame and no
 *      CPU to take away, so there is no killing to do, and the honest answer is
 *      `-WC_EPERM`. A program that could end its own parent would be able to
 *      orphan everything else on this screen, which is a capability nobody has
 *      earned yet.
 *
 * SPDX-License-Identifier: MIT
 */
#include "wcstr.h"

void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    long pid = wc_getpid();

    char line[64];
    unsigned n;

    n = 0;
    n = wc_app_str(line, n, "[whoami] pid ");
    n = wc_app_signed(line, n, pid);
    n = wc_app_str(line, n, "\n");
    wc_write(STDOUT_FILENO, line, n);

    long r = wc_kill(1, SIGKILL);
    n = 0;
    n = wc_app_str(line, n, "[whoami] kill 1 -> ");
    n = wc_app_signed(line, n, r);
    n = wc_app_str(line, n, "\n");
    wc_write(STDOUT_FILENO, line, n);

    wc_puts("[whoami] the shell that started me is not killable\n");
    wc_exit(0);
}
