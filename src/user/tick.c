/* WindClone — /bin/tick: the smallest program that proves a scheduler.
 *
 * hello, cat and echo all run to completion in one go; nothing about them
 * needs the CPU to be shared. This one is different on purpose: it prints a
 * line, gives the CPU back with sched_yield(), and continues — over and over.
 * Two copies of it, spawned as "A" and "B", make the whole test: with a real
 * round-robin scheduler their lines come out interleaved in the serial log,
 * A-1 B-1 A-2 B-2 ..., and each keeps its own counter across the gaps where
 * the other ran. If scheduling were broken they would run straight through one
 * after the other, or one would never resume — and interleaving is exactly what
 * a single-program runner cannot produce.
 *
 * The label comes from argv[1], not from a hard-coded name, because the two
 * instances are the same binary: what tells them apart is only the command line
 * the loader handed each process, and reading it back is a free re-proof that
 * argv crossed the privilege boundary intact.
 *
 * Every line is composed in a buffer and written with ONE write syscall: a
 * timer tick can preempt this process between syscalls but never mid-syscall,
 * so one write per line keeps the marker a whole line for CI to grep.
 *
 * SPDX-License-Identifier: MIT
 */
#include "wcstr.h"

void _start(int argc, char **argv)
{
    const char *label = argc > 1 ? argv[1] : "tick";
    unsigned long len = wc_strlen(label);
    long pid = wc_getpid();

    for (int i = 1; i <= 3; i++) {
        char line[64];
        unsigned n = 0;
        n = wc_app_str(line, n, "[tick ");
        n = wc_app_bytes(line, n, label, len);
        n = wc_app_str(line, n, "] step ");
        n = wc_app_dec(line, n, (unsigned long)i);
        n = wc_app_str(line, n, " pid ");
        n = wc_app_dec(line, n, (unsigned long)(pid < 0 ? 0 : pid));
        n = wc_app_str(line, n, "\n");
        wc_write(STDOUT_FILENO, line, n);

        /* Give the CPU up and pick the counter back up where it stopped. */
        wc_sched_yield();
    }

    char line[64];
    unsigned n = 0;
    n = wc_app_str(line, n, "[tick ");
    n = wc_app_bytes(line, n, label, len);
    n = wc_app_str(line, n, "] done\n");
    wc_write(STDOUT_FILENO, line, n);
    wc_exit(0);
}
