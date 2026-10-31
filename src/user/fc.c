/* WindClone — /bin/fc: cmd's File Compare, over both halves of the file layer.
 *
 * The store now has DIR, DEL, REN and MOVE answering at the prompt and /bin/copy
 * moving bytes between the archive and the store. The verb that completes that
 * little family is the one Windows reaches for before it edits anything: `fc`
 * says whether two files are the same and, when they are not, exactly where
 * they stopped agreeing. Like copy, and unlike the built-ins, this one *reads*,
 * and reading reaches the read-only initrd too — so `fc /bin/hello /tmp/hello`
 * is the question a person actually asks after a copy: did the bytes survive?
 *
 * Why both files are read whole before one byte is compared: same rule as the
 * copier's. A run that discovers midway it cannot finish has to leave the same
 * answer a full run would have given, and here nothing is written at all, so the
 * stronger promise is that a refusal never half-compares — both paths are
 * opened and slurped, or the program says which one failed and stops.
 *
 * Why the first difference is a byte offset and a line number: cmd's fc locates
 * the divergence in *lines* because a person reads text, but a line number alone
 * is a claim about newline bytes, and this compare works on any file — a BMP, a
 * zip member, a note. So the offset is the fact (the machine's answer) and the
 * line is the reading of that fact (the human's answer), both on one line CI
 * can grep and a user can act on.
 *
 * Why the exit code is cmd's: 0 identical, 1 different, 2 unreadable or too
 * big, 1 a wrong command line. fc is the first program here whose *exit code*
 * is the answer rather than a crash status, and the shell's own
 * "shell: fc exited with N" reap line is what proves it lands.
 *
 * The words on screen are French, like every other window in this OS. The lines
 * it logs are English, because they are the lines CI greps.
 *
 * SPDX-License-Identifier: MIT
 */
#include "wcstr.h"

/* The store's ceiling per file (see /bin/copy's note on the same constant):
 * anything a program wrote is at most this, and an archive file bigger than it
 * is refused with its real size, never truncated into a false verdict. */
#define FC_MAX          4096
#define FC_PATH_MAX     40
#define FC_CHUNK        512

#define FC_EXIT_SAME    0
#define FC_EXIT_DIFF    1
#define FC_EXIT_USAGE   2   /* wrong number of words, or a refused path */
#define FC_EXIT_NOFILE  2   /* one side could not be opened or read */
#define FC_EXIT_BIG     2   /* one side is larger than this program can hold */

static char a_buf[FC_MAX], b_buf[FC_MAX];
static char path[2][FC_PATH_MAX];
static char line[64];
static char chunk[FC_CHUNK];

/* Same contract as copy's: absolute, letters digits _ . / - only, no "..". */
static int valid_path(int slot, const char *s)
{
    unsigned n = 0;

    if (s[0] != '/')
        return 0;
    for (; s[n] != '\0'; n++) {
        char c = s[n];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '_' || c == '.' ||
                 c == '/' || c == '-';
        if (!ok)
            return 0;
        if (c == '.' && s[n + 1] == '.')
            return 0;
        if (n + 1 >= FC_PATH_MAX)
            return 0;
        path[slot][n] = c;
    }
    if (n == 1)
        return 0;
    path[slot][n] = '\0';
    return 1;
}

/* One line, one write() — see wcstr.h: a tick may land between two syscalls
 * and tear a line across another process's output, and CI greps whole lines. */
static void say_len(unsigned n)
{
    line[n] = '\n';
    wc_write(STDOUT_FILENO, line, n + 1);
}

static void say(const char *s)
{
    say_len(wc_app_str(line, 0, s));
}

static void say_pid(unsigned n)
{
    n = wc_app_str(line, n, " pid ");
    n = wc_app_dec(line, n, (unsigned long)wc_getpid());
    say_len(n);
}

/* Read `p` whole into `buf`. Returns the length, -1 unopenable/unreadable,
 * -2 bigger than FC_MAX (with the real size said, never a guess). */
static long slurp(const char *p, char *buf)
{
    long fd = wc_open(p);
    unsigned long len = 0;

    if (fd < 0) {
        unsigned n = wc_app_str(line, 0, "fc: cannot open ");
        n = wc_app_str(line, n, p);
        n = wc_app_str(line, n, ", error ");
        n = wc_app_signed(line, n, fd);
        say_pid(n);
        return -1;
    }
    for (;;) {
        unsigned long room = FC_MAX - len;
        long got;

        if (room == 0) {
            unsigned long extra = 0;
            long more = wc_read(fd, chunk, FC_CHUNK);

            while (more > 0) {
                extra += (unsigned long)more;
                more = wc_read(fd, chunk, FC_CHUNK);
            }
            wc_close(fd);
            if (more < 0) {
                unsigned n = wc_app_str(line, 0, "fc: read failed on ");
                n = wc_app_str(line, n, p);
                n = wc_app_str(line, n, ", error ");
                n = wc_app_signed(line, n, more);
                say_pid(n);
                return -1;
            }
            if (extra == 0)
                break;                    /* exactly FC_MAX, which fits */
            {
                unsigned n = wc_app_str(line, 0, "fc: too big, ");
                n = wc_app_dec(line, n, FC_MAX + extra);
                n = wc_app_str(line, n, " bytes at ");
                n = wc_app_str(line, n, p);
                say_pid(n);
            }
            return -2;
        }
        if (room > FC_CHUNK)
            room = FC_CHUNK;
        got = wc_read(fd, chunk, room);
        if (got < 0) {
            unsigned n;

            wc_close(fd);
            n = wc_app_str(line, 0, "fc: read failed on ");
            n = wc_app_str(line, n, p);
            n = wc_app_str(line, n, ", error ");
            n = wc_app_signed(line, n, got);
            say_pid(n);
            return -1;
        }
        if (got == 0)
            break;
        for (long i = 0; i < got; i++)
            buf[len + (unsigned long)i] = chunk[i];
        len += (unsigned long)got;
    }
    wc_close(fd);
    return (long)len;
}

void _start(int argc, char **argv)
{
    long la, lb;
    unsigned long n, min, lines, at_lines, diff_at;
    unsigned long da, db;

    if (argc < 3) {
        say("fc: usage fc <fichier1> <fichier2>");
        wc_exit(FC_EXIT_USAGE);
    }
    if (!valid_path(0, argv[1]) || !valid_path(1, argv[2])) {
        say("fc: a path is absolute, letters digits _ . / - only, no ..");
        wc_exit(FC_EXIT_USAGE);
    }

    la = slurp(path[0], a_buf);
    if (la < 0)
        wc_exit(FC_EXIT_NOFILE);
    lb = slurp(path[1], b_buf);
    if (lb < 0)
        wc_exit(FC_EXIT_NOFILE);

    /* Both are in hand: say what each one is before saying whether they agree.
     * Sizes and digests first, so a verdict line can never be the only thing a
     * reader has to go on. Paths do not fit next to two hex digests inside a
     * 56-byte line — they are on the compare line's own report above it. */
    {
        unsigned n2 = wc_app_str(line, 0, "fc: ");
        n2 = wc_app_str(line, n2, path[0]);
        n2 = wc_app_str(line, n2, " vs ");
        n2 = wc_app_str(line, n2, path[1]);
        say_pid(n2);
    }
    da = wc_fnv1a(a_buf, (unsigned long)la);
    db = wc_fnv1a(b_buf, (unsigned long)lb);
    {
        unsigned n2 = wc_app_str(line, 0, "fc: a=");
        n2 = wc_app_dec(line, n2, (unsigned long)la);
        n2 = wc_app_str(line, n2, "b=");
        n2 = wc_app_dec(line, n2, (unsigned long)lb);
        n2 = wc_app_str(line, n2, " bytes 0x");
        n2 = wc_app_hex(line, n2, da);
        n2 = wc_app_str(line, n2, "/0x");
        n2 = wc_app_hex(line, n2, db);
        say_len(n2);
    }

    min = (unsigned long)(la < lb ? la : lb);
    diff_at = min;                          /* "no difference within the min" */
    for (n = 0; n < min; n++)
        if (a_buf[n] != b_buf[n]) {
            diff_at = n;
            break;
        }

    /* The line the first difference sits on: count newline bytes strictly
     * BEFORE the position, then add one — a reader's line numbers start at 1.
     * We cannot use count_lines here because that helper includes an "unfinished
     * trailing line" in its count, which would double the +1 below. */
    lines = 0;
    for (n = 0; n < diff_at; n++)
        if (a_buf[n] == '\n')
            lines++;
    at_lines = lines + 1;

    if ((unsigned long)la == (unsigned long)lb && diff_at == min) {
        say("fc: no differences encountered");
        wc_exit(FC_EXIT_SAME);
    }

    if ((unsigned long)la != (unsigned long)lb) {
        unsigned n2 = wc_app_str(line, 0, "fc: sizes differ by ");
        unsigned long d = (unsigned long)la > (unsigned long)lb
                            ? (unsigned long)la - (unsigned long)lb
                            : (unsigned long)lb - (unsigned long)la;
        n2 = wc_app_dec(line, n2, d);
        n2 = wc_app_str(line, n2, " bytes");
        say_len(n2);
    }
    {
        unsigned n2 = wc_app_str(line, 0, "fc: first difference at byte ");
        n2 = wc_app_dec(line, n2, diff_at);
        n2 = wc_app_str(line, n2, " line ");
        n2 = wc_app_dec(line, n2, at_lines);
        say_len(n2);
    }
    say("fc: comparison failed");           /* cmd's own phrase, lower case */
    wc_exit(FC_EXIT_DIFF);
}
