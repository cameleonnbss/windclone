/* WindClone — /bin/hold: the process that keeps the screen alive.
 *
 * Every other scheduler program finishes its business inside the one excursion
 * the shell used to make into ring 3. That is exactly the case that hid the bug
 * this release fixes: because a launch ran the whole group to completion before
 * the idle loop turned over again, a program that *kept running* froze the
 * desktop — no repaint, no mouse, nothing but the last frame it drew. `hold`
 * is deliberately built to run long enough to fall into that gap and prove it
 * closed: twenty rounds, each one a line and a yield and a short busy stretch,
 * so the process is still going across several idle-loop iterations while its
 * output streams into the terminal.
 *
 * It only observes, never decides — the same rule /bin/nest follows. Which round
 * a repaint happened to land between is scheduling luck (the visit budget and the
 * tick decide it), so the honest claim is "these rounds exist, in order, with
 * this pid, and a frame painted between two of them", which the test reads out
 * of the serial log without pinning any round to any tick. The exit code, on the
 * other hand, is fixed at 5 on purpose: it arrives later than the launch, from
 * the reap on the visit that finally runs this program to its end, and that
 * ordering is the whole point.
 *
 * The busy stretch between yields is not decoration. A round that only yielded
 * would let the cooperative path hand the CPU off; the delay means a tick can
 * land mid-computation, so `hold` exercises the preempt arm of the same visit
 * budget — the one that lets even a process that never calls sched_yield() give
 * the compositor its slice back.
 *
 * SPDX-License-Identifier: MIT
 */
#include "wcstr.h"

#define HOLD_ROUNDS 20

/* The exit code /bin/hold leaves for the shell to reap. Nothing computes it; the
 * claim is simply that it comes back through the async reap path, not at launch. */
#define HOLD_EXIT 5

/* A short busy delay, in loop iterations. Tuned so twenty rounds take long
 * enough on QEMU's emulated clock to span a few idle-loop paints, and short
 * enough that a headless run is not held up. `volatile` keeps the compiler from
 * deleting the loop as dead, which is the whole reason it exists. */
static void hold_busy(void)
{
    volatile int n = 300000;
    while (n > 0)
        n--;
}

void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    long me = wc_getpid();

    for (int i = 1; i <= HOLD_ROUNDS; i++) {
        char line[64];
        unsigned n = 0;
        n = wc_app_str(line, n, "[hold] round ");
        n = wc_app_dec(line, n, (unsigned long)i);
        n = wc_app_str(line, n, " pid ");
        n = wc_app_signed(line, n, me);
        n = wc_app_str(line, n, "\n");
        wc_write(STDOUT_FILENO, line, n);

        hold_busy();
        wc_sched_yield();
    }

    char line[64];
    unsigned n = 0;
    n = wc_app_str(line, n, "[hold] exiting pid ");
    n = wc_app_signed(line, n, me);
    n = wc_app_str(line, n, " code ");
    n = wc_app_dec(line, n, HOLD_EXIT);
    n = wc_app_str(line, n, "\n");
    wc_write(STDOUT_FILENO, line, n);

    wc_exit(HOLD_EXIT);
}
