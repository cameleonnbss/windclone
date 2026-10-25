/* WindClone — /bin/exit: a child with a message for its parent.
 *
 * The wait() demo needs children that die with a *known* exit code, taken from
 * the command line — which makes this tiny program a proof of two milestones at
 * once: the code it exits with is an argv[1] that crossed the privilege boundary
 * through the kernel's bounce buffers, and the pid it prints is what the parent
 * must get back from wait() for the reaping check to line up.
 *
 * Prints one composed line and exits; there is deliberately no yield and no
 * loop, so the exit syscall lands while the parent is blocked in wait() and the
 * wake-up path is the only way the parent can resume.
 *
 * SPDX-License-Identifier: MIT
 */
#include "wcstr.h"

/* argv[1] is text; decimal -> int, one byte at a time. No strtol in userland
 * and none needed: the codes here are small non-negative numbers. */
static unsigned long wc_parse_dec(const char *s)
{
    unsigned long v = 0;
    for (; *s >= '0' && *s <= '9'; s++)
        v = v * 10 + (unsigned long)(*s - '0');
    return v;
}

void _start(int argc, char **argv)
{
    unsigned long code = argc > 1 ? wc_parse_dec(argv[1]) : 0;
    long pid = wc_getpid();

    char line[64];
    unsigned n = 0;
    n = wc_app_str(line, n, "[exit] code ");
    n = wc_app_dec(line, n, code);
    n = wc_app_str(line, n, " pid ");
    n = wc_app_dec(line, n, (unsigned long)(pid < 0 ? 0 : pid));
    n = wc_app_str(line, n, "\n");
    wc_write(STDOUT_FILENO, line, n);

    wc_exit((int)code);
}
