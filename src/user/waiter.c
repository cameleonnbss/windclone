/* WindClone — /bin/waiter: the process trilogy completed, in one program.
 *
 * hello proved a program can *run*. tick proved the CPU can be *shared*. spin
 * proved the sharing can be *forced*. What none of them could show is a process
 * that *sleeps* until something happens: every call so far returned into the
 * same process that made it. This one blocks.
 *
 * It spawns two children with real command lines (/bin/exit 7 and /bin/exit 9,
 * argv copied through the kernel's bounce buffers into a private address space),
 * then calls wait() twice. Each wait() puts this process to sleep as BLOCKED —
 * the CPU goes to the children, which is the only way they can ever run to
 * their exits — and wakes here only when a child dies, carrying back that
 * child's pid and exit code. The printed pairs are the evidence:
 *
 *     [waiter] spawned pid 5 code 7
 *     [exit] code 7 pid 5
 *     [waiter] reaped pid 5 code 7
 *
 * reaped pid == spawned pid and reaped code == the argv[1] we passed means
 * spawn-with-argv, blocking wait, the wake-up, and zombie reaping all worked
 * together. A fake "wait" that never blocked could print the right numbers by
 * luck of scheduling; it could not survive the second call with the first
 * child's slot already reclaimed to FREE.
 *
 * SPDX-License-Identifier: MIT
 */
#include "wcstr.h"

/* One child per row: the binary, and the code its argv asks it to exit with. */
static const char *codes[2] = { "7", "9" };

void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    long kids[2];
    for (int i = 0; i < 2; i++) {
        char *kargv[2];
        kargv[0] = "exit";
        kargv[1] = (char *)codes[i];
        kids[i] = wc_spawn_args("/bin/exit", 2, kargv);

        char line[64];
        unsigned n = 0;
        n = wc_app_str(line, n, "[waiter] spawned pid ");
        n = wc_app_dec(line, n, (unsigned long)(kids[i] < 0 ? 0 : kids[i]));
        n = wc_app_str(line, n, " code ");
        n = wc_app_str(line, n, codes[i]);
        n = wc_app_str(line, n, "\n");
        wc_write(STDOUT_FILENO, line, n);
    }

    for (int i = 0; i < 2; i++) {
        int code = -1;
        long pid = wc_wait(&code);

        char line[64];
        unsigned n = 0;
        n = wc_app_str(line, n, "[waiter] reaped pid ");
        n = wc_app_dec(line, n, (unsigned long)(pid < 0 ? 0 : pid));
        n = wc_app_str(line, n, " code ");
        n = wc_app_dec(line, n, (unsigned long)(code < 0 ? 0 : code));
        n = wc_app_str(line, n, "\n");
        wc_write(STDOUT_FILENO, line, n);
    }

    wc_puts("[waiter] all children reaped\n");
    wc_exit(0);
}
