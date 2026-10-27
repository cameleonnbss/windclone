/* WindClone — /bin/keys: the program that waits for the keyboard.
 *
 * Until now a program could print but never listen: fd 0 answered -ENOSYS and
 * every byte IRQ1 produced went to the shell, so the only way to "type to a
 * program" was to type a command that *started* it. This is the smallest program
 * that proves the other half exists, and it is deliberately not an editor — an
 * editor is the release this unlocks, and building one on top of an unproven read
 * would hide which half is wrong when the screen stays blank.
 *
 * What it does is one read(0) in a loop, which is where the interesting part
 * lives. The first call claims the keyboard: the kernel makes this process the
 * owner and, finding no keys waiting, puts it to *sleep* rather than answering
 * 0 — the idle loop repaints the desktop and polls the hardware while it waits,
 * and the keystroke that lands is what wakes it. A program that sleeps costs no
 * CPU, which is the claim a poll loop could never make.
 *
 * The lines it writes are the test's vocabulary, and each one is one syscall so a
 * tick cannot tear it mid-line:
 *
 *   keys: waiting for keys pid N      — it asked, and it is asleep
 *   keys: got N bytes pid N           — one chunk arrived (how many is luck)
 *   keys: line 'TEXT' len N pid N     — the line the user meant
 *   keys: lost the keyboard pid N     — someone else took the keys away
 *
 * The chunk count is deliberately not asserted: how many bytes one read returns
 * depends on when the typist's fingers land between scheduler visits, and a test
 * that pinned it would be a test of QEMU's timing. The *line* is the promise —
 * every byte typed before Enter, in order — and that holds however the chunks
 * arrive. So the program accumulates until a newline and prints what it collected.
 *
 * It ends on its own after that line, which is the second half of the proof: the
 * exit hands the keyboard back to the console, and the prompt has to work again
 * without anyone clicking. A program that swallowed the keyboard when it died
 * would leave a desktop you can look at but cannot use.
 *
 * SPDX-License-Identifier: MIT
 */
#include "wcstr.h"

/* The longest line kept. The appenders cap a printed line at 56 bytes, so a
 * longer collection would be silently cut in half by its own echo; 32 leaves the
 * wrapper ("keys: line '…' len N pid N") comfortably inside that cap. */
#define KEYS_LINE_MAX 32

/* How many keys one read() may take at most. Anything up to this arrives in one
 * call if the typist was quick — and a headless scenario typing one key per
 * scheduler visit gets exactly one, which is why the program counts *bytes* and
 * not reads: eight chunks is not eight keys, and a ceiling on chunks would be a
 * ceiling on how fast somebody is allowed to type. */
#define KEYS_CHUNK   32

/* Exit codes, one per way this program can end, because the window outlives the
 * process and the exit code it shows is the only thing saying *why*. */
#define KEYS_EXIT_LINE    0
#define KEYS_EXIT_READERR 1
#define KEYS_EXIT_LOST    3
#define KEYS_EXIT_LONG    4

void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    long me = wc_getpid();
    char out[64];
    unsigned n;

    n = 0;
    n = wc_app_str(out, n, "keys: waiting for keys pid ");
    n = wc_app_signed(out, n, me);
    n = wc_app_str(out, n, "\n");
    wc_write(STDOUT_FILENO, out, n);

    char line[KEYS_LINE_MAX];
    unsigned len = 0;

    for (;;) {
        char chunk[KEYS_CHUNK];
        long got = wc_read(STDIN_FILENO, chunk, sizeof chunk);

        if (got < 0) {
            n = 0;
            n = wc_app_str(out, n, "keys: read failed ");
            n = wc_app_signed(out, n, got);
            n = wc_app_str(out, n, " pid ");
            n = wc_app_signed(out, n, me);
            n = wc_app_str(out, n, "\n");
            wc_write(STDOUT_FILENO, out, n);
            wc_exit(KEYS_EXIT_READERR);
        }

        if (got == 0) {
            /* A blocked read answered zero bytes: the window manager says this
             * program is no longer the one on top, so the keys belong to someone
             * else now. Not an error — a polite hand-back — but a reason to stop
             * rather than to re-claim the keyboard from under the prompt. */
            n = 0;
            n = wc_app_str(out, n, "keys: lost the keyboard pid ");
            n = wc_app_signed(out, n, me);
            n = wc_app_str(out, n, "\n");
            wc_write(STDOUT_FILENO, out, n);
            wc_exit(KEYS_EXIT_LOST);
        }

        n = 0;
        n = wc_app_str(out, n, "keys: got ");
        n = wc_app_dec(out, n, (unsigned long)got);
        n = wc_app_str(out, n, " bytes pid ");
        n = wc_app_signed(out, n, me);
        n = wc_app_str(out, n, "\n");
        wc_write(STDOUT_FILENO, out, n);

        for (long i = 0; i < got; i++) {
            if (chunk[i] == '\n') {
                n = 0;
                n = wc_app_str(out, n, "keys: line '");
                n = wc_app_bytes(out, n, line, len);
                n = wc_app_str(out, n, "' len ");
                n = wc_app_dec(out, n, len);
                n = wc_app_str(out, n, " pid ");
                n = wc_app_signed(out, n, me);
                n = wc_app_str(out, n, "\n");
                wc_write(STDOUT_FILENO, out, n);
                wc_exit(KEYS_EXIT_LINE);
            }
            if (len < KEYS_LINE_MAX)
                line[len++] = chunk[i];
        }

        if (len >= KEYS_LINE_MAX) {
            /* Full, and no newline in it. The ceiling is the line and not the
             * number of reads, because a typist who sends one key per scheduler
             * visit produces one read per key: bound the reads and the program
             * would give up in the middle of a word, measuring how fast the
             * keyboard is typed rather than what it was asked to collect. */
            n = 0;
            n = wc_app_str(out, n, "keys: no newline after ");
            n = wc_app_dec(out, n, len);
            n = wc_app_str(out, n, " bytes pid ");
            n = wc_app_signed(out, n, me);
            n = wc_app_str(out, n, "\n");
            wc_write(STDOUT_FILENO, out, n);
            wc_exit(KEYS_EXIT_LONG);
        }
    }
}
