/* WindClone — /bin/reaper: the parent that ends its own child.
 *
 * /bin/waiter showed a parent that *sleeps* until a child dies on its own. This
 * is the other half of process control: a parent that decides *when*. Together
 * they are the four verbs a Unix process table is for — spawn, exit, wait, kill
 * — and this program runs all four in one pass, in the order the kernel needs:
 *
 *   1. spawn /bin/bell, a child that will run for seconds and never quit by
 *      itself, and print the pid the kernel gave back;
 *   2. yield a few times so the bell really runs — "[bell] round 1" appearing
 *      is what makes "it was alive" an observation rather than an assumption;
 *   3. kill(pid, SIGKILL) and print what the call answered;
 *   4. wait(&code) and print what came back. The reap must name the SAME pid
 *      and carry code -9: the negative signal number is the kernel's own
 *      marking, not something this program could have written.
 *
 * Then two error paths, because a table that cannot say "no such process" is a
 * table a program can poke at blindly: killing the pid a moment ago reaped
 * answers -WC_ESRCH (-3), and a signal other than SIGKILL answers -WC_EINVAL
 * (-22) — there is no signal delivery in this kernel, so promising one would be
 * a lie.
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

    char *kargv[1] = { "bell" };
    long kid = wc_spawn_args("/bin/bell", 1, kargv);

    char line[64];
    unsigned n;

    n = 0;
    n = wc_app_str(line, n, "[reaper] spawned pid ");
    n = wc_app_signed(line, n, kid);
    n = wc_app_str(line, n, "\n");
    wc_write(STDOUT_FILENO, line, n);

    /* Hand the CPU over so the child can run and prove it was alive. Three
     * yields, not one: the round-robin may land on the bell at any point in
     * its own loop, and we want its first line on the log before we decide its
     * fate. */
    for (int i = 0; i < 3; i++)
        wc_sched_yield();

    long r = wc_kill(kid, SIGKILL);
    n = 0;
    n = wc_app_str(line, n, "[reaper] kill ");
    n = wc_app_signed(line, n, kid);
    n = wc_app_str(line, n, " -> ");
    n = wc_app_signed(line, n, r);
    n = wc_app_str(line, n, "\n");
    wc_write(STDOUT_FILENO, line, n);

    int code = -1;
    long reaped = wc_wait(&code);
    n = 0;
    n = wc_app_str(line, n, "[reaper] reaped pid ");
    n = wc_app_signed(line, n, reaped);
    n = wc_app_str(line, n, " code ");
    n = wc_app_signed(line, n, code);
    n = wc_app_str(line, n, "\n");
    wc_write(STDOUT_FILENO, line, n);

    /* The child is reaped — its slot is FREE — so its pid is dead ground. */
    long again = wc_kill(kid, SIGKILL);
    n = 0;
    n = wc_app_str(line, n, "[reaper] kill again -> ");
    n = wc_app_signed(line, n, again);
    n = wc_app_str(line, n, "\n");
    wc_write(STDOUT_FILENO, line, n);

    /* Only SIGKILL exists: an answer other than -WC_EINVAL would mean the
     * kernel accepted a signal it has no way to deliver. */
    long bogus = wc_kill(kid, 1);
    n = 0;
    n = wc_app_str(line, n, "[reaper] kill sig 1 -> ");
    n = wc_app_signed(line, n, bogus);
    n = wc_app_str(line, n, "\n");
    wc_write(STDOUT_FILENO, line, n);

    /* And nobody would be waiting, so the last wait() must say so instead of
     * sleeping forever — the difference between a scheduler and a hang. */
    long orphan = wc_wait(&code);
    n = 0;
    n = wc_app_str(line, n, "[reaper] wait again -> ");
    n = wc_app_signed(line, n, orphan);
    n = wc_app_str(line, n, "\n");
    wc_write(STDOUT_FILENO, line, n);

    wc_puts("[reaper] child killed and reaped\n");
    wc_exit(0);
}
