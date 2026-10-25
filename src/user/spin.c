/* WindClone — /bin/spin: the program that proves preemption.
 *
 * /bin/tick proves the cooperative scheduler: it prints a line, calls
 * sched_yield(), and lets the next process have the CPU. That test cannot tell
 * a real preemptive scheduler from a run-to-completion loop, because the switch
 * only ever happens where the program *asks* for it. This one is the opposite:
 * it does a long, hot compute loop and NEVER yields. Nothing here volunteers
 * the CPU back. So if two copies still interleave, it can only be because the
 * timer interrupt forced a switch out from under a running process — which is
 * exactly what preemption means, and what a cooperative-only scheduler could
 * never do (A would spin all the way to the end before B ever printed).
 *
 * Each round burns a fixed number of counted iterations, then prints one
 * progress line. That line is the witness: its very existence mid-compute,
 * appearing between the other process's lines, shows the tick landed while this
 * process was running and the scheduler moved on. The `spin` accumulator is
 * volatile so -O2 cannot delete the loop it is meant to measure.
 *
 * Every line is composed in a buffer and written with ONE write syscall: a
 * tick can preempt this process between syscalls but never mid-syscall (ring 0
 * is not preempted here), so one write per line keeps each marker a whole line
 * for CI to grep instead of letting a switch tear "[spin B" across "round 2".
 *
 * SPDX-License-Identifier: MIT
 */
#include "wcstr.h"

/* Long enough that the 100 Hz timer (one tick every 10 ms) is certain to fire
 * between two progress lines on any QEMU host, short enough that two whole
 * runs still finish well inside the CI boot timeout. */
#define WORK_PER_ROUND 30000000ULL
#define ROUNDS         5

void _start(int argc, char **argv)
{
    const char *label = argc > 1 ? argv[1] : "spin";
    unsigned long len = wc_strlen(label);
    long pid = wc_getpid();

    {
        char line[64];
        unsigned n = 0;
        n = wc_app_str(line, n, "[spin ");
        n = wc_app_bytes(line, n, label, len);
        n = wc_app_str(line, n, "] start pid ");
        n = wc_app_dec(line, n, (unsigned long)(pid < 0 ? 0 : pid));
        n = wc_app_str(line, n, "\n");
        wc_write(STDOUT_FILENO, line, n);
    }

    volatile unsigned long spin = 0;
    for (int r = 1; r <= ROUNDS; r++) {
        for (unsigned long i = 0; i < WORK_PER_ROUND; i++)
            spin += i;

        char line[64];
        unsigned n = 0;
        n = wc_app_str(line, n, "[spin ");
        n = wc_app_bytes(line, n, label, len);
        n = wc_app_str(line, n, "] round ");
        n = wc_app_dec(line, n, (unsigned long)r);
        n = wc_app_str(line, n, "\n");
        wc_write(STDOUT_FILENO, line, n);
    }

    {
        char line[64];
        unsigned n = 0;
        n = wc_app_str(line, n, "[spin ");
        n = wc_app_bytes(line, n, label, len);
        n = wc_app_str(line, n, "] done\n");
        wc_write(STDOUT_FILENO, line, n);
    }
    wc_exit(0);
}
