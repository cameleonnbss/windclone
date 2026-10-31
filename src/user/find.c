/* WindClone — /bin/find: cmd's string search, over the file layer.
 *
 * fc answers "are these two files the same?"; the verb a person reaches for
 * before that is the quieter one: "is this line in here at all?" Windows'
 * `find` is exactly that — it prints every line of a file that carries a
 * piece of text, and its exit code says whether any line did. This program is
 * that verb, grown onto the same reading path fc already proved: it opens each
 * named file through the VFS, so a search reaches both halves of the layer —
 * the read-only initrd (`find int /src/hello.c`) and the writable store
 * (`find two /tmp/fnd.bat`) — with the same open() the copier and comparator
 * use.
 *
 * Why the flags are cmd's and only cmd's common ones: /i folds case the way
 * `find /i` does, /n prefixes each hit with its line number the way `find /n`
 * does, and /v prints the lines that *don't* match, the way `find /v` does.
 * /c (count only) and /off are left out — this store has no directories, so
 * there is no "the first line is offline header" to skip, and the summary line
 * already carries the count. A flag is two characters, a slash and a letter:
 * anything longer is not a flag, so a path like /src/hello.c is never mistaken
 * for one and dropped from the search.
 *
 * Why the exit code is cmd's, like fc's: 0 if at least one line was printed
 * (found, or — under /v — at least one line lacked the text), 1 if nothing was
 * printed, 2 for a wrong command line or a file that cannot be read or is too
 * big. The shell's "shell: find exited with N" reap line is what proves the
 * code lands, and it is the answer the caller acts on, not a crash status.
 *
 * Why a refusal stops the whole run rather than skipping one file: same rule
 * as the comparator's. A search that quietly dropped an unreadable file would
 * report "0 matches" for a text that *is* there, and "not found" is a claim
 * about files it never actually looked at. So the first file that cannot be
 * opened, or is bigger than this program holds, ends the run with a refusal and
 * exit 2 — never a verdict built on a file that was skipped.
 *
 * The words on screen are French, like every other window in this OS. The
 * lines it logs are English, because they are the lines CI greps.
 *
 * SPDX-License-Identifier: MIT
 */
#include "wcstr.h"

/* The store's ceiling per file, the same FC_MAX the comparator holds: anything
 * a program wrote is at most this, and an archive file bigger than it is
 * refused with its real size, never truncated into a false "not found". */
#define FIND_MAX        4096
#define FIND_PATH_MAX   40
#define FIND_CHUNK      512
#define FIND_PAT_MAX    64

#define FIND_EXIT_FOUND   0
#define FIND_EXIT_NONE    1
#define FIND_EXIT_USAGE   2   /* no pattern, or no file, or a refused path */
#define FIND_EXIT_NOFILE  2   /* a file could not be opened or read */
#define FIND_EXIT_BIG     2   /* a file is larger than this program can hold */

static char buf[FIND_MAX];
static char path[FIND_PATH_MAX];
static char line[64];
static char chunk[FIND_CHUNK];
static char pat[FIND_PAT_MAX];
static unsigned pat_len;

/* Case-insensitive, show line numbers, print the non-matching lines. Parsed
 * from the leading /x flags, so they are file-scope only to save passing them
 * through every helper. */
static int icase, numbers, invert;

/* One ASCII case fold. The only upper-to-lower gap that matters here is A-Z;
 * everything else passes through untouched, which is what a byte search wants.
 * cmd's find /i folds exactly this range and nothing wider. */
static char fold(char c)
{
    if (c >= 'A' && c <= 'Z')
        return (char)(c + 32);
    return c;
}

/* Absolute path, letters digits _ . / - only, no "..". */
static int valid_path(const char *s)
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
        if (n + 1 >= FIND_PATH_MAX)
            return 0;
        path[n] = c;
    }
    if (n == 1)
        return 0;
    path[n] = '\0';
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

/* Does the run of buf starting at `start`, `len` bytes long, carry the
 * pattern? A plain byte-for-byte substring test, folding both sides when /i is
 * set. Empty pattern or one longer than the line can never match, so those
 * short-circuit — a zero-length search string would otherwise "match" every
 * line, which is not what anyone typing `find ""` means. */
static int line_has(unsigned start, unsigned len)
{
    unsigned i, j;

    if (pat_len == 0 || pat_len > len)
        return 0;
    for (i = 0; i + pat_len <= len; i++) {
        for (j = 0; j < pat_len; j++) {
            char a = buf[start + i + j];
            char b = pat[j];
            if (icase) {
                a = fold(a);
                b = fold(b);
            }
            if (a != b)
                break;
        }
        if (j == pat_len)
            return 1;
    }
    return 0;
}

/* Echo one line: "find: " then (with /n) the line number and ": ", then the
 * bytes, capped at 56 like every appender — a long source line is truncated in
 * the *display*, never in the *match*, which was decided over the full bytes. */
static void print_line(unsigned ln, unsigned start, unsigned len)
{
    unsigned m = wc_app_str(line, 0, "find: ");
    if (numbers) {
        m = wc_app_dec(line, m, ln);
        m = wc_app_str(line, m, ": ");
    }
    m = wc_app_bytes(line, m, buf + start, len);
    say_len(m);
}

/* Read `p` whole into buf. Returns the length, -1 unopenable/unreadable (with
 * the failing path and error said), -2 bigger than FIND_MAX (with the real
 * size said). */
static long slurp(const char *p)
{
    long fd = wc_open(p);
    unsigned long len = 0;

    if (fd < 0) {
        unsigned n = wc_app_str(line, 0, "find: cannot open ");
        n = wc_app_str(line, n, p);
        n = wc_app_str(line, n, ", error ");
        n = wc_app_signed(line, n, fd);
        say_pid(n);
        return -1;
    }
    for (;;) {
        unsigned long room = FIND_MAX - len;
        long got;

        if (room == 0) {
            unsigned long extra = 0;
            long more = wc_read(fd, chunk, FIND_CHUNK);

            while (more > 0) {
                extra += (unsigned long)more;
                more = wc_read(fd, chunk, FIND_CHUNK);
            }
            wc_close(fd);
            if (more < 0) {
                unsigned n = wc_app_str(line, 0, "find: read failed on ");
                n = wc_app_str(line, n, p);
                n = wc_app_str(line, n, ", error ");
                n = wc_app_signed(line, n, more);
                say_pid(n);
                return -1;
            }
            if (extra == 0)
                break;                    /* exactly FIND_MAX, which fits */
            {
                unsigned n = wc_app_str(line, 0, "find: too big, ");
                n = wc_app_dec(line, n, FIND_MAX + extra);
                n = wc_app_str(line, n, " bytes at ");
                n = wc_app_str(line, n, p);
                say_pid(n);
            }
            return -2;
        }
        if (room > FIND_CHUNK)
            room = FIND_CHUNK;
        got = wc_read(fd, chunk, room);
        if (got < 0) {
            unsigned n;

            wc_close(fd);
            n = wc_app_str(line, 0, "find: read failed on ");
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

/* Search one already-opened file. `len` is the byte count in buf; prints the
 * header, then every matching line, and returns how many lines were printed.
 * A line is the bytes up to the next '\n'; the newline itself is not part of
 * the line and not searched, so a pattern ending in a newline never matches —
 * the bare-token grammar cannot type one anyway. A trailing run with no final
 * newline is still one line, and cmd counts it too. */
static unsigned long scan_one(unsigned long len)
{
    unsigned long printed = 0, lineno = 0;
    unsigned i = 0, start = 0;
    int hit;

    {
        unsigned n = wc_app_str(line, 0, "find: ");
        n = wc_app_str(line, n, path);
        say_len(n);
    }
    while (i < len) {
        if (buf[i] == '\n') {
            lineno++;
            hit = line_has(start, i - start);
            if ((hit ^ invert) != 0) {
                print_line(lineno, start, i - start);
                printed++;
            }
            i++;
            start = i;
        } else {
            i++;
        }
    }
    if (start < len) {                    /* last line, no trailing newline */
        lineno++;
        hit = line_has(start, (unsigned)len - start);
        if ((hit ^ invert) != 0) {
            print_line(lineno, start, (unsigned)len - start);
            printed++;
        }
    }
    return printed;
}

void _start(int argc, char **argv)
{
    int i;
    unsigned long total = 0;

    icase = numbers = invert = 0;

    /* Leading /x flags: exactly a slash and one letter. A longer token ("/abc")
     * is not a flag and is left for the pattern/file parse, so paths are safe. */
    i = 1;
    while (i < argc && argv[i][0] == '/' && argv[i][1] != '\0'
           && argv[i][2] == '\0') {
        char f = fold(argv[i][1]);
        if (f == 'i')
            icase = 1;
        else if (f == 'n')
            numbers = 1;
        else if (f == 'v')
            invert = 1;
        else
            break;                         /* unknown switch: stop, treat as text */
        i++;
    }

    /* Need a pattern AND at least one file: argv[i] is the pattern, argv[i+1]
     * the first file. Anything less is a wrong command line. */
    if (i + 1 >= argc) {
        say("find: usage find [/i] [/n] [/v] chaine fichier...");
        wc_exit(FIND_EXIT_USAGE);
    }

    pat_len = 0;
    {
        const char *p = argv[i];
        while (p[pat_len] != '\0' && pat_len < FIND_PAT_MAX - 1) {
            pat[pat_len] = p[pat_len];
            pat_len++;
        }
        pat[pat_len] = '\0';
    }
    i++;                                   /* first file argument */

    for (; i < argc; i++) {
        long len;

        if (!valid_path(argv[i])) {
            say("find: a path is absolute, letters digits _ . / - only, no ..");
            wc_exit(FIND_EXIT_USAGE);
        }
        len = slurp(path);
        if (len < 0)
            wc_exit(FIND_EXIT_NOFILE);     /* refusal, never a skipped file */
        total += scan_one((unsigned long)len);
    }

    {
        unsigned n = wc_app_str(line, 0, "find: ");
        n = wc_app_dec(line, n, total);
        n = wc_app_str(line, n, " matches");
        say_len(n);
    }
    wc_exit(total > 0 ? FIND_EXIT_FOUND : FIND_EXIT_NONE);
}
