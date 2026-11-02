/* WindClone — /bin/where: cmd's "which program answers this name?".
 *
 * The shell already resolves a typed word to /bin/<name> silently, so the
 * question "WHERE did that command come from?" has never had an answer here.
 * cmd answers it with `where`: it searches the scopes it knows (PATH, then the
 * current directory) and prints the full path of every match, raw — one line
 * per hit, no decoration — because those lines are meant to be fed to the next
 * command, not read by a human. This store has exactly one scope, /bin, so the
 * search is honest rather than simplified: one entry per program, and the
 * printed path is the one open() accepts and certutil can hash.
 *
 * Why it reads the directory rather than a hardcoded list: the archive is the
 * only place that knows what is installed. A where that recited a list written
 * into this file would lie the moment a release adds a program — and v0.1.46
 * is precisely the release that would have caught it, because certutil appeared
 * after the list would have been frozen. Same rule the start menu already
 * follows: walk SYS_GETDIRENT, and the answer follows the store.
 *
 * Why the comparison ignores case: cmd's file matching does, on every version
 * of Windows ever shipped. `where CERTUTIL` finds /bin/certutil here just like
 * it finds C:\Windows\System32\certutil.exe there. The paths printed come from
 * the archive verbatim, so they open exactly as printed.
 *
 * Exit codes are where.exe's published shape: 0 when at least one pattern
 * matched, 1 when nothing did ("INFO: Could not find files for the given
 * pattern(s)." is what Windows prints for that, and it is a report, not a
 * crash), and 2 for a command line that cannot work — no criteria at all, or a
 * switch. There are no switches here, and refusing one loudly is what keeps
 * that true: an accepted-but-ignored /R would promise a recursive search this
 * flat store cannot perform.
 *
 * The words on screen are French, like every other window in this OS. The lines
 * it logs are English, because they are the lines CI greps — and the found
 * lines are the raw paths, because that is what Windows prints.
 *
 * SPDX-License-Identifier: MIT
 */
#include "wcstr.h"

#define WH_LINE_MAX   64
#define WH_EXIT_OK        0   /* at least one pattern matched */
#define WH_EXIT_NOTFOUND  1   /* cmd's "nothing matched" answer */
#define WH_EXIT_USAGE     2   /* no criteria, or a refused switch */

static char line[WH_LINE_MAX];

/* One whole line in one syscall: the path is the payload, so nothing may tear
 * it across an interleaved log line. */
static void emit_line(void)
{
    unsigned n = wc_strlen(line);

    line[n] = '\n';
    wc_write(STDOUT_FILENO, line, n + 1);
}

static char lc(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Emit one of this program's own literals whole. The shared appenders cap at
 * 56 bytes to protect a caller's buffer from argv, but these strings are
 * counted at compile time and short, so the cap would only truncate CI's
 * grep target — "…pattern(s)." losing its last characters, for instance. */
static void emit_str(const char *s)
{
    unsigned n = 0;

    while (s[n] != '\0' && n < WH_LINE_MAX - 2) {
        line[n] = s[n];
        n++;
    }
    line[n] = '\0';
    emit_line();
}

/* A search word has to be a bare name: letters, digits, _ . - only. A slash
 * would ask where to find a *path*, which is not a question this one-scope
 * store can answer, and wildcard characters would promise matching where.exe
 * really does and this program does not. */
static int valid_word(const char *s)
{
    unsigned n = 0;

    if (s[0] == '\0')
        return 0;
    for (; s[n] != '\0'; n++) {
        char c = s[n];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-'))
            return 0;
        if (n >= 48)
            return 0;
    }
    return 1;
}

/* Case-insensitive "does this archive entry name a program called `word`".
 * Entries carry no leading slash — "bin/certutil" — so the comparison starts
 * at the scope this store actually has. */
static int matches(const char *name, const char *word)
{
    int i;

    for (i = 0; i < 4; i++)
        if (lc(name[i]) != "bin/"[i])
            return 0;
    name += 4;
    for (;;) {
        char c = lc(*name);
        if (c != lc(*word))
            return 0;
        if (c == '\0')
            return 1;
        name++;
        word++;
    }
}

static void say_missing_criteria(void)
{
    emit_str("ERROR: Missing search criteria.");
    emit_str("WHERE Pattern1 [Pattern2 ...]");
}

void _start(int argc, char **argv)
{
    int found_any = 0;
    int i;

    if (argc < 2) {
        say_missing_criteria();
        wc_exit(WH_EXIT_USAGE);
    }

    /* Refuse switches and paths before matching anything: cmd checks the whole
     * command line's shape first, and a half-run search is worse than none. */
    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '/') {
            unsigned n = wc_app_str(line, 0, "ERROR: Invalid switch - ");
            n = wc_app_str(line, n, argv[i]);
            n = wc_app_str(line, n, ".");
            line[n] = '\0';
            emit_line();
            wc_exit(WH_EXIT_USAGE);
        }
        if (!valid_word(argv[i])) {
            unsigned n = wc_app_str(line, 0, "ERROR: Invalid pattern - ");
            n = wc_app_str(line, n, argv[i]);
            line[n] = '\0';
            emit_line();
            wc_exit(WH_EXIT_USAGE);
        }
    }

    /* One pass per pattern, matches printed as found, in the order the caller
     * asked. The store is small and the archive walk is a syscall loop; the
     * same walk the start menu does on every click. */
    for (i = 1; i < argc; i++) {
        unsigned long idx = 0;
        int hit = 0;

        for (;;) {
            struct wc_dirent de;

            if (wc_getdirent(idx, &de) < 0)
                break;
            idx++;
            if (!matches(de.name, argv[i]))
                continue;
            {
                unsigned n = wc_app_str(line, 0, "/");
                n = wc_app_bytes(line, n, de.name, wc_strnlen(de.name, 63));
                line[n] = '\0';
                emit_line();
            }
            hit = 1;
        }
        if (hit)
            found_any = 1;
    }

    if (!found_any) {
        emit_str("INFO: Could not find files for the given pattern(s).");
        wc_exit(WH_EXIT_NOTFOUND);
    }
    wc_exit(WH_EXIT_OK);
}
