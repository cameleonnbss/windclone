/* WindClone — /bin/echo: the smallest program that proves argv.
 *
 * ls and cat read the kernel. This one reads its own command line, which is
 * the part of the handover the loader added for Phase 5: the shell tokenised
 * the line, usermode_run() copied the words onto the program's stack, and
 * rdi/rsi arrived at _start holding them. If any of that were wrong this
 * program prints nothing, prints the wrong words, or faults in ring 3 — and
 * "echo zzz" coming back as "zzz" is the whole proof.
 *
 * It is deliberately the plain behaviour: arguments separated by single
 * spaces, one newline at the end, no -n and no escapes. Those are a shell's
 * job to grow later, and each one is a promise this file would have to keep.
 *
 * SPDX-License-Identifier: MIT
 */
#include "wcstr.h"

static const char space = ' ';
static const char newline = '\n';

void _start(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (i > 1)
            wc_write(STDOUT_FILENO, &space, 1);
        wc_write(STDOUT_FILENO, argv[i], wc_strlen(argv[i]));
    }
    wc_write(STDOUT_FILENO, &newline, 1);

    wc_exit(0);
}
