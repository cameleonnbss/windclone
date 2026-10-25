/* WindClone — /bin/vanish: the case kill() cannot handle in place.
 *
 * A kill aimed at another process is a table edit — the victim is not on the
 * CPU, so the kernel can retire it where it stands. A kill aimed at *yourself*
 * is the opposite: you are holding the CPU, the CR3, and the trap frame the
 * syscall is about to iretq through, so the only honest implementation is the
 * exit arm of the scheduler switch. That call does not return, and this program
 * is the demonstration.
 *
 * The evidence is again an absence: "[vanish] bye" is on the log, "[vanish]
 * STILL HERE" never is. If self-kill had been written as "return 0 and let the
 * program continue", the second line would print — which is exactly the bug
 * this two-line program exists to catch.
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
    unsigned n = 0;
    n = wc_app_str(line, n, "[vanish] bye pid ");
    n = wc_app_dec(line, n, (unsigned long)(pid < 0 ? 0 : pid));
    n = wc_app_str(line, n, "\n");
    wc_write(STDOUT_FILENO, line, n);

    /* Never comes back: the kernel takes the exit path with code -SIGKILL. */
    wc_kill(pid, SIGKILL);

    n = 0;
    n = wc_app_str(line, n, "[vanish] STILL HERE kill returned ");
    n = wc_app_str(line, n, "\n");
    wc_write(STDOUT_FILENO, line, n);
    wc_exit(0);
}
