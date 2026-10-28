/* WindClone — /bin/view: the first program that colours its own window.
 *
 * Note proved a program can own a screen. View asks a harder question: can a
 * program own a screen that means something, where a reader can see the
 * difference between a keyword and an identifier without counting columns?
 * Answering it needed one new idea and nothing else, and the new idea is a
 * byte.
 *
 * Why an escape byte and not a syscall. A colour request could have been
 * `wc_color(fd, index)`, and the release would have needed a new ABI number, a
 * new kernel entry, a new struct in the contract every program shares. Instead
 * a program writes FS (0x1C, the byte ASCII reserved for "file separator" and
 * that no document text has ever wanted) followed by a digit, and every glyph
 * after it is painted in that ink. It is in-band for exactly the reason the
 * form feed is (see shell.c's surf_write): the request travels on the same wire
 * as the text it decorates, so it arrives in the same write() as the row it
 * belongs to and cannot be reordered against it. One frame, one syscall, ten
 * colours if there were ten — the byte costs a cell of nothing because it is
 * not a cell.
 *
 * Why eight inks and not RGB. The kernel keeps one byte per cell, so a window
 * costs 17x62 bytes of attribute instead of 17x62 words, and a program can only
 * name a role — keyword, string, number, comment — not a hue. That is the right
 * shape anyway: what makes a highlighter readable is that `int` and `return`
 * are coloured for the same reason, and a viewer that picked raw RGB would be
 * free to disagree with itself about what those reasons are. Index 0 is the
 * green every cell starts in, so a program that never mentions a colour paints
 * exactly the screen it painted before colour existed. That is not a
 * compatibility note for old programs, it is the proof the escaping is honest.
 *
 * Why view is read-only. The mouse can address a cell in a program window and
 * the keyboard reaches a program that is on top, so writing a file is possible
 * — but a highlighter that could also delete a user's source with one wrong key
 * would need a dirty flag, an undo, and a save dialog, and none of those are
 * this program's job. View is the half of a code editor that is genuinely
 * harder than it looks: reading a file, deciding what each token *is*, and
 * saying so on screen while somebody scrolls. Editing came in note and stays
 * there.
 *
 * The words on screen are French, like every other window in this OS. The
 * lines it logs are English, because they are the lines CI greps.
 *
 * SPDX-License-Identifier: MIT
 */
#include "wcstr.h"

#define VIEW_MAX       2048           /* the whole file, in RAM          */
#define VIEW_PATH_MAX  40

/* The frame is the surface a program window paints: 17 rows, 62 cells each.
 * Rows are capped at 56 cells so a long line of source cannot wrap itself into
 * the status row, and the gutter that numbers the lines is part of that budget
 * rather than extra to it: 4 digits, a space, then up to 51 cells of text. */
#define VIEW_ROWS        17
#define VIEW_BODY_ROWS   12
#define VIEW_GUTTER      5
#define VIEW_LINE_MAX    56
#define VIEW_CODE_MAX    (VIEW_LINE_MAX - VIEW_GUTTER)

/* Worst case per row: an escape before every cell. 2 bytes of escape plus 1 of
 * glyph is 3 cells' worth of bytes per column, and the cap above makes that
 * 168 bytes a row; 12 rows, a title, a status line, a key line and the form
 * feed fit in what is left. The frame never exceeds this, and it is the write
 * buffer's own limit that keeps it true rather than an argument. */
#define VIEW_FRAME_MAX   3072

/* The palette, by role. These numbers are the indices in shell.c's color_ink[]
 * and nothing here can check that: the table is the kernel's, the meaning is
 * the program's, and the test that ties them together is that the colours
 * appear on the screen where this file says they should. */
#define C_TEXT     0      /* the default green: identifiers, punctuation */
#define C_WHITE    1      /* emphasis: the file name in the title        */
#define C_KEYWORD  2      /* if, for, return ...                         */
#define C_STRING   3      /* "..." and '...'                             */
#define C_NUMBER   4      /* 42, 0x1C, 8                               */
#define C_ERROR    5      /* a name that failed, an unmatched quote     */
#define C_TYPE     6      /* int, char, void, struct ...                 */
#define C_COMMENT  7      /* both kinds, and the line-number gutter      */

#define VIEW_ESC   0x1C

#define K_HOME   1    /* Ctrl+A — first line                  */
#define K_PAGEUP 2    /* Ctrl+B — one screen up               */
#define K_END    5    /* Ctrl+E — last page                   */
#define K_PAGEDN 6    /* Ctrl+F — one screen down             */
#define K_BS     8
#define K_TAB    9
#define K_LF    10
#define K_DOWN  14    /* Ctrl+N — one line down               */
#define K_UP    16    /* Ctrl+P — one line up                 */
#define K_QUIT  17    /* Ctrl+Q — close the viewer            */

#define VIEW_EXIT_OK      0
#define VIEW_EXIT_USAGE   2   /* a path that cannot be one, or an open that failed */
#define VIEW_EXIT_READ    3   /* read(0) answered an error */
#define VIEW_EXIT_LOST    4   /* another window took the keyboard */

static char src[VIEW_MAX];
static unsigned slen;                 /* bytes of source */
static unsigned top;                  /* first source line shown */
static unsigned nlines;               /* lines in the file, for the status row */
static unsigned nkey;                 /* keyword tokens, the claim it worked */

static uint8_t cls[VIEW_MAX];         /* one palette index per byte of src */

static char path[VIEW_PATH_MAX];
static char line[64];
static char frame[VIEW_FRAME_MAX];
static unsigned fn;

/* --- log lines --------------------------------------------------------
 * One line, one write(), the rule every program here follows: a switch cannot
 * happen inside a syscall, so a line written in one call is a line a test can
 * find whole. */
static void say_len(unsigned n)
{
    line[n] = '\n';
    wc_write(STDOUT_FILENO, line, n + 1);
}

static void say(const char *s)
{
    say_len(wc_app_str(line, 0, s));
}

static void say_pid(unsigned n, long pid)
{
    n = wc_app_str(line, n, " pid ");
    n = wc_app_dec(line, n, (unsigned long)pid);
    say_len(n);
}

/* --- the highlighter --------------------------------------------------
 * Two word lists and a scanner. They are C's because that is the language this
 * OS is written in and the file it opens; a real editor's lexer is a generated
 * table driven by a grammar, and a generated table is not something a freestanding
 * program can carry. What is deliberately *not* done: no nesting, no macro
 * expansion, no context. A token is what its bytes say in isolation, which is
 * the same rule a human highlighting by hand follows, and it is honest about
 * its one known lie — a word inside a comment is coloured as a comment, which
 * is what a reader wants and not what a compiler does. */
static const char *const keywords[] = {
    "if", "else", "while", "for", "do", "return", "break", "continue",
    "switch", "case", "default", "goto", "static", "const", "extern",
    "sizeof", "volatile", "inline", "define", "undef", "include", 0
};

static const char *const types[] = {
    "int", "char", "void", "long", "short", "float", "double",
    "unsigned", "signed", "struct", "union", "enum", "typedef",
    "size_t", "uint8_t", "bool", 0
};

static int is_word(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* Exact, case-sensitive, bounded by the token's own length: `w` points into
 * src[] and holds exactly n bytes, so a table entry longer than the token
 * cannot read past it. */
static int matches(const char *s, const char *w, unsigned n)
{
    unsigned i = 0;

    while (i < n && s[i] != '\0') {
        if (s[i] != w[i])
            return 0;
        i++;
    }
    return i == n && s[i] == '\0';
}

static int word_class(unsigned start, unsigned n)
{
    for (int i = 0; keywords[i] != 0; i++)
        if (matches(keywords[i], src + start, n))
            return C_KEYWORD;
    for (int i = 0; types[i] != 0; i++)
        if (matches(types[i], src + start, n))
            return C_TYPE;
    return C_TEXT;
}

static void set_class(unsigned start, unsigned end, int a)
{
    for (unsigned i = start; i < end; i++)
        cls[i] = (uint8_t)a;
}

/* One pass over the whole file, once, at open. Painting then costs nothing:
 * drawing a row is a lookup per byte rather than a re-lex, which is what lets a
 * repaint happen on every keypress with no thought about how long the file is.
 * nkey is counted here, so "highlighted N keywords" is a fact about the file
 * and not about the twelve rows currently on screen. */
static void classify(void)
{
    unsigned i = 0;

    nkey = 0;
    for (unsigned k = 0; k < slen; k++)
        cls[k] = C_TEXT;

    while (i < slen) {
        char c = src[i];

        if (c == '/' && i + 1 < slen && src[i + 1] == '/') {
            unsigned end = i;
            while (end < slen && src[end] != '\n')
                end++;
            set_class(i, end, C_COMMENT);
            i = end;
            continue;
        }
        if (c == '/' && i + 1 < slen && src[i + 1] == '*') {
            unsigned end = i + 2;
            while (end + 1 < slen && !(src[end] == '*' && src[end + 1] == '/'))
                end++;
            /* Unclosed, the comment runs to the end of the file. A block
             * comment that never terminates is a bug in the specimen, and the
             * honest response is to stop painting at the file rather than read
             * past it looking for a terminator that is not there. */
            end = end + 1 < slen ? end + 2 : slen;
            set_class(i, end, C_COMMENT);
            i = end;
            continue;
        }
        if (c == '"' || c == '\'') {
            unsigned end = i + 1;
            while (end < slen && src[end] != c && src[end] != '\n')
                end++;
            if (end < slen && src[end] == c) {
                set_class(i, end + 1, C_STRING);
                i = end + 1;          /* include the closing quote */
                continue;
            }
            /* No close on this line: paint what there is in the error ink and
             * carry on, because a string that does not end is exactly the kind
             * of thing a viewer should make visible instead of hiding. */
            set_class(i, end, C_ERROR);
            i = end;
            continue;
        }
        if (c >= '0' && c <= '9') {
            unsigned end = i;
            while (end < slen && is_word(src[end]))
                end++;
            set_class(i, end, C_NUMBER);
            i = end;
            continue;
        }
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
            unsigned end = i;
            while (end < slen && is_word(src[end]))
                end++;
            int a = word_class(i, end - i);
            if (a == C_KEYWORD)
                nkey++;
            set_class(i, end, a);
            i = end;
            continue;
        }
        i++;
    }
}

/* --- the frame --------------------------------------------------------
 * Built in one buffer, written in one syscall, so a window changes in one step
 * instead of showing half an old frame over half a new one. Two appends
 * matter: f_addc counts a cell, f_esc does not, because the kernel consumes
 * FS+digit before it reaches the grid. Capping cells and not bytes is the
 * whole difference between a row that cannot wrap and a buffer that cannot
 * overflow — both are enforced here. */
static unsigned row_cells;

static void f_esc(int a)
{
    if (fn + 2 >= VIEW_FRAME_MAX)
        return;
    frame[fn++] = (char)VIEW_ESC;
    frame[fn++] = (char)('0' + a);
}

static void f_addc(char c)
{
    if (row_cells >= VIEW_LINE_MAX || fn + 1 >= VIEW_FRAME_MAX)
        return;
    frame[fn++] = c;
    row_cells++;
}

static void f_add(const char *s)
{
    while (*s)
        f_addc(*s++);
}

static void f_addn(unsigned long v)
{
    char d[24];
    unsigned n = 0;

    if (v == 0)
        d[n++] = '0';
    while (v > 0 && n < sizeof d) {
        d[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0)
        f_addc(d[--n]);
}

static void f_begin(void)
{
    row_cells = 0;
}

static void f_end(void)
{
    if (fn + 1 < VIEW_FRAME_MAX)
        frame[fn++] = '\n';
}

/* A byte of source as a glyph. The 5x7 font has no tab and no control picture,
 * so both become the blank they would be anyway. The file's bytes are never
 * touched — this is only what a human can see. */
static char show_byte(char c)
{
    if (c == '\t')
        return ' ';
    if (c >= 32 && c < 127)
        return c;
    return ' ';
}

/* One source line, coloured. `n` is its line number, counted from 1, and the
 * gutter is part of the row's cell budget rather than a decoration outside it:
 * a line number that could push source off the right edge would be a way to
 * make the viewer lie about the file. */
static void f_code_row(unsigned start, unsigned end, unsigned n)
{
    int cur = -1;
    unsigned col = 0;

    f_begin();
    f_esc(C_COMMENT);
    f_addn(n);
    while (row_cells < VIEW_GUTTER)
        f_addc(' ');

    for (unsigned i = start; i < end && col < VIEW_CODE_MAX; i++, col++) {
        int a = cls[i];
        if (a != cur) {
            f_esc(a);
            cur = a;
        }
        f_addc(show_byte(src[i]));
    }
    f_end();
}

static void draw(void)
{
    unsigned i, painted, ln;
    unsigned last;

    fn = 0;
    f_begin();                        /* the form feed is not a cell, so it gets
                                       * a row of its own to be counted in */
    f_addc('\f');                     /* a screen, not another line */
    f_end();

    f_begin();
    f_esc(C_WHITE);
    f_add("VIEW - ");
    f_add(path);
    f_end();

    /* The frame is exactly 17 rows: form feed, title, blank, twelve rows of
     * source, blank, status, keys. Twelve because that is what a 240-pixel
     * window holds at 11 pixels a row, and exactly rather than at most because
     * a frame whose status row can land on two different rows is a frame no
     * headless test can assert on. */
    f_begin();
    f_end();

    /* Walk to the first line the view shows, then paint twelve. */
    for (ln = 0, i = 0; ln < top && i < slen; ln++) {
        while (i < slen && src[i] != '\n')
            i++;
        if (i < slen)
            i++;
    }

    painted = 0;
    while (painted < VIEW_BODY_ROWS && i < slen) {
        unsigned start = i;

        while (i < slen && src[i] != '\n')
            i++;
        f_code_row(start, i, top + painted + 1);
        painted++;
        if (i < slen)
            i++;
    }
    last = top + painted;             /* the highest line number on screen */

    /* Keep every frame exactly 17 rows. A frame shorter than the screen would
     * leave the previous frame's bottom rows on the surface, and a viewer whose
     * status line moves is a viewer whose status line cannot be tested. */
    while (painted < VIEW_BODY_ROWS) {
        f_begin();
        f_end();
        painted++;
        last = top + painted;         /* a padded row is still a row the view
                                       * claims to be showing: the status line
                                       * has to agree with the screen */
    }

    f_begin();
    f_end();

    f_begin();
    f_esc(C_WHITE);
    f_add("LIGNES ");
    f_addn(top + 1);
    f_add("-");
    f_addn(last > nlines ? nlines : last);
    f_add(" SUR ");
    f_addn(nlines);
    f_add("  ");
    f_esc(C_KEYWORD);
    f_addn(nkey);
    f_add(" MOTS-CLES");
    f_end();

    f_begin();
    f_esc(C_TEXT);
    f_add("N/P=LIGNE  B/F=PAGE  A/E=DEBUT-FIN  Q=QUITTER (CTRL)");
    f_end();

    wc_write(STDOUT_FILENO, frame, fn);
}

/* --- the keys --------------------------------------------------------- */

static void scroll_lines(long delta)
{
    long last = (long)nlines - (long)VIEW_BODY_ROWS;

    if (last < 0)
        last = 0;
    long t = (long)top + delta;
    if (t < 0)
        t = 0;
    if (t > last)
        t = last;
    top = (unsigned)t;
}

static int count_lines(void)
{
    unsigned n = 1;

    if (slen == 0)
        return 0;                     /* an empty file has no lines at all */
    for (unsigned i = 0; i < slen; i++)
        if (src[i] == '\n')
            n++;
    return (int)n;
}

/* --- the path ---------------------------------------------------------
 * A path, not a name: view opens whatever is in the archive or the store, so it
 * validates the shape rather than rebuilding the whole string. Letters, digits
 * and _ . / - only, and no "..": the kernel resolves exact names and could not
 * be fooled, but a viewer that refuses a traversal is a viewer a person can
 * point at a file and trust. */
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
        if (c == '.') {
            /* Two dots in a row is the only shape that could mean "go up", and
             * it is refused on the string rather than on a resolved path: the
             * kernel matches exact names, so nothing here could actually be
             * fooled, and a refusal the reader can see is worth more than a
             * traversal the reader has to trust. */
            if (s[n + 1] == '.')
                return 0;
        }
    }
    if (n == 1 || n >= VIEW_PATH_MAX)
        return 0;

    for (unsigned i = 0; i < n; i++)
        path[i] = s[i];
    path[n] = '\0';
    return 1;
}

static int load(long pid)
{
    long fd = wc_open(path);

    if (fd < 0) {
        unsigned n = wc_app_str(line, 0, "view: cannot open ");
        n = wc_app_str(line, n, path);
        n = wc_app_str(line, n, ", error ");
        n = wc_app_signed(line, n, fd);
        say_pid(n, pid);
        return 1;
    }

    while (slen < VIEW_MAX) {
        long got = wc_read(fd, src + slen, VIEW_MAX - slen);
        if (got <= 0)
            break;
        slen += (unsigned)got;
    }
    wc_close(fd);

    nlines = (unsigned)count_lines();
    classify();
    return 0;
}

void _start(int argc, char **argv)
{
    long me = wc_getpid();
    const char *want;

    if (argc < 2) {
        say("view: usage is view <path>");
        wc_exit(VIEW_EXIT_USAGE);
    }
    want = argv[1];
    if (!valid_path(want)) {
        unsigned n = wc_app_str(line, 0, "view: bad path ");
        n = wc_app_bytes(line, n, want, wc_strnlen(want, 20));
        say_len(n);
        say("view: a path is absolute, letters digits _ . / - only");
        wc_exit(VIEW_EXIT_USAGE);
    }

    if (load(me) != 0)
        wc_exit(VIEW_EXIT_USAGE);

    {
        unsigned n = wc_app_str(line, 0, "view: opened ");
        n = wc_app_str(line, n, path);
        n = wc_app_str(line, n, ", ");
        n = wc_app_dec(line, n, slen);
        n = wc_app_str(line, n, " bytes");
        /* Said out loud rather than kept private: a viewer that quietly showed
         * the first half of a long file is a viewer whose line count is a lie,
         * and the log is where a test can catch it. */
        if (slen >= VIEW_MAX)
            n = wc_app_str(line, n, ", truncated");
        say_pid(n, me);
    }
    {
        unsigned n = wc_app_str(line, 0, "view: highlighted ");
        n = wc_app_dec(line, n, nkey);
        n = wc_app_str(line, n, " keywords in ");
        n = wc_app_dec(line, n, nlines);
        n = wc_app_str(line, n, " lines");
        say_pid(n, me);
    }

    draw();

    for (;;) {
        char chunk[32];
        long got = wc_read(STDIN_FILENO, chunk, sizeof chunk);

        if (got < 0) {
            unsigned n = wc_app_str(line, 0, "view: keyboard read failed ");
            n = wc_app_signed(line, n, got);
            say_pid(n, me);
            wc_exit(VIEW_EXIT_READ);
        }
        if (got == 0) {
            /* Zero bytes from a blocked read means the window manager no longer
             * says this program is the one on top: the keys belong to somebody
             * else now, so stop rather than take them back. */
            unsigned n = wc_app_str(line, 0, "view: lost the keyboard");
            say_pid(n, me);
            wc_exit(VIEW_EXIT_LOST);
        }

        int moved = 0;

        for (long i = 0; i < got; i++) {
            int c = (unsigned char)chunk[i];

            if (c == K_QUIT) {
                unsigned n = wc_app_str(line, 0, "view: quit");
                say_pid(n, me);
                wc_exit(VIEW_EXIT_OK);
            }
            if (c == K_DOWN) {
                scroll_lines(1);
                moved = 1;
            } else if (c == K_UP) {
                scroll_lines(-1);
                moved = 1;
            } else if (c == K_PAGEDN) {
                scroll_lines(VIEW_BODY_ROWS);
                moved = 1;
            } else if (c == K_PAGEUP) {
                scroll_lines(-(long)VIEW_BODY_ROWS);
                moved = 1;
            } else if (c == K_HOME) {
                top = 0;
                moved = 1;
            } else if (c == K_END) {
                scroll_lines((long)nlines);
                moved = 1;
            }
            /* Any other byte leaves the view alone. A viewer that repainted on
             * a key it does not use would cost a syscall for nothing and, worse,
             * would put a frame on the wire that the test did not ask for. */
        }
        if (moved)
            draw();                   /* one frame per read, not one per byte */
    }
}
