/* WindClone — /bin/note: the first program that edits a file.
 *
 * Three releases built the parts this needs and none of them was an editor.
 * v0.1.18 gave write() somewhere to go, so a byte a program made could outlive
 * the program. v0.1.20 gave a program the keyboard, so a byte a person made
 * could reach a program. And the form feed below gives a program a *screen*
 * instead of an ever-scrolling list of everything it has ever printed. Note is
 * what you get when those three are wired together: a window you can type into,
 * text that changes as you type, and a file that says the change really landed.
 *
 * Why it is not Notepad. A Windows-style editor needs a mouse-driven caret, a
 * selection, and an open dialog drawn as a second window — none of which a
 * ring-3 program here can ask for yet. What it does have is the whole of what a
 * text editor is made of: a buffer, a caret that moves in two dimensions,
 * insert and delete, a dirty flag, a save, and a file. The keys are the honest
 * limit: the PS/2 decoder puts letters, punctuation, space, Enter, Backspace,
 * Tab and the Ctrl letters on the wire, and the arrow keys are not among them,
 * so movement uses the Ctrl letters that have named those motions since the
 * VT100 (B/F/P/N, A/E) rather than pretending to have something it does not.
 *
 * How the screen is drawn. Every edit repaints the whole frame: one write()
 * starting with \f, of a title row, twelve body rows, a status row and a key
 * row. One write per frame is what makes the window change in one step instead
 * of showing half an old line and half a new one, and \f is what keeps twelve
 * repaints from becoming 200 lines of scrollback. This is also why the caret is
 * drawn as a '|' inside the text: a program window has no hardware cursor of
 * its own, and a caret that is part of the frame is part of what the log can
 * prove was on the screen.
 *
 * The words the frame shows are French, because the screen the rest of this OS
 * draws is French. The lines it logs are English, because they are the same log
 * the kernel, the shell and every other program here write for CI to grep, and
 * one vocabulary for machine-readable lines is worth more than a bilingual one.
 * Those log lines are the observable contract: one syscall each, so a tick
 * cannot tear one across another process's output.
 *
 * What v0.1.23 joined: the two halves that used to live in separate programs.
 * Note had the buffer, the caret and the save; /bin/view had the classifier. A
 * file now reaches the editor under its own name — `note work.c` opens
 * /tmp/work.c, and the one dot a name may carry is what makes that possible —
 * the screen paints the same roles the viewer paints, out of the same table, and
 * Ctrl+S writes the bytes back.
 *
 * And with that came a hazard a note never had: holding less than the file
 * holds. A 512-byte buffer could not meet a big source file; a 2048-byte one
 * can. So a document that did not fit says (TRONQUE) on the row a person reads
 * first, and its save is refused. Writing half a file back over a whole one is
 * the one failure an editor is not allowed to have, and it is the kind of
 * failure that looks like success — the store would report a clean write,
 * because it would be telling the truth about the bytes it was given.
 *
 * SPDX-License-Identifier: MIT
 */
#include "wcstr.h"
#include "highlight.h"

#define NOTE_DIR         "/tmp/"
#define NOTE_EXT         ".txt"
#define NOTE_NAME_MAX    12             /* letters, digits or _, one dot        */
#define NOTE_PATH_MAX    32
/* One document, in RAM. 2048 because that is what /bin/view already holds, and
 * an editor whose buffer is smaller than its viewer is an editor that cannot
 * open the files it claims to edit. It is *not* the store's limit: TMPFS_FILE_MAX
 * is 4096, so a file too big for this buffer is a real thing, and load() below
 * notices rather than quietly showing half of it. */
#define NOTE_DOC_MAX     2048

/* The frame is the surface a program window paints: 17 rows, and the kernel
 * wraps anything longer than 62 columns. Rows are capped at 56 so a document
 * line can never wrap itself into the next row and eat a row of the layout. */
#define NOTE_ROWS        17
#define NOTE_BODY_ROWS   12
#define NOTE_LINE_MAX    56
/* Worst case, and the reason it grew with the colour: a row of 56 cells can
 * change ink on every cell, and an escape is two bytes that cost no cell — so
 * 3 bytes per column, 168 a row, 12 rows of body, plus title, status, keys and
 * the blanks. NOTE_FRAME_MAX is 3072 with room to spare, and the caps in
 * f_addc()/f_esc() enforce it rather than the arithmetic proving it. */
#define NOTE_FRAME_MAX   3072

/* What the decoder hands a program when Ctrl is held with a letter: the byte
 * that letter names, exactly as every PC keyboard has meant it since the IBM
 * XT. These are the editor's commands, and they are the reason the arrow keys
 * not being on the wire is an inconvenience rather than a blocker. */
#define K_HOME   1    /* Ctrl+A — start of the line          */
#define K_LEFT   2    /* Ctrl+B — one character back         */
#define K_DEL    4    /* Ctrl+D — delete under the caret     */
#define K_END    5    /* Ctrl+E — end of the line            */
#define K_RIGHT  6    /* Ctrl+F — one character forward      */
#define K_BS     8    /* Backspace — delete before the caret */
#define K_TAB    9    /* a literal tab: the one whitespace a decoder
                       * can reach that is not a space       */
#define K_LF    10    /* Enter — break a line at the caret   */
#define K_DOWN  14    /* Ctrl+N — one line down              */
#define K_UP    16    /* Ctrl+P — one line up                */
#define K_QUIT  17    /* Ctrl+Q — close the editor           */
#define K_SAVE  19    /* Ctrl+S — write the document out     */

/* One exit code per way this program can end, because the window outlives the
 * process and the "EXIT n" the compositor paints under the last frame is the
 * only thing left saying why it stopped. */
#define NOTE_EXIT_OK      0
#define NOTE_EXIT_SAVE    1   /* a save was asked for and did not happen */
#define NOTE_EXIT_USAGE   2   /* a name that cannot be a file, or an open that failed */
#define NOTE_EXIT_READ    3   /* read(0) answered an error */
#define NOTE_EXIT_LOST    4   /* another window took the keyboard */
#define NOTE_EXIT_DIRTY   7   /* quit with edits nobody saved */

static char doc[NOTE_DOC_MAX];
static unsigned dlen;             /* bytes of document           */
static unsigned dpos;             /* the caret, 0..dlen          */
static unsigned dtop;             /* first document line shown   */
static int dirty;                 /* 1 = the edits are not in the file */
static int truncated;             /* 1 = the file is bigger than doc[] */
static int colour;                /* 1 = this name is source, paint it */
static unsigned nkey;             /* keyword tokens in the document    */

/* One palette index per byte of the document, filled by highlight.h on every
 * repaint. It is a separate array and not a spare field in the character,
 * because a byte of text has no room for a colour and a colour must not cost a
 * byte of text: the document keeps its exact bytes, which is what lets a save
 * write back the file it read plus the keys typed into it and nothing else. */
static unsigned char cls[NOTE_DOC_MAX];

static char path[NOTE_PATH_MAX];
static char line[64];             /* one log line's worth */
static char frame[NOTE_FRAME_MAX];
static unsigned fn;
static char probe[64];            /* the load() overflow read, see truncated */

/* --- log lines --------------------------------------------------------
 * One line, one write(), the rule every program here follows: a switch cannot
 * happen inside a syscall, so a line written in one call is a line a test can
 * find whole. wcstr.h's appenders cap at 56 bytes, so a path that is somehow
 * longer than its own budget cannot overrun `line` — it gets cut, and the cut
 * is visible in the log as a name that stops short. */
static void say_len(unsigned n)
{
    line[n] = '\n';
    wc_write(STDOUT_FILENO, line, n + 1);
}

static void say(const char *s)
{
    say_len(wc_app_str(line, 0, s));
}

/* Close a line with the pid that is saying it. Every program in /bin ends its
 * lines this way: a window can hold a finished process and a live one with the
 * same name, and the pid is the only thing that says which wrote this. */
static void say_pid(unsigned n, long pid)
{
    n = wc_app_str(line, n, " pid ");
    n = wc_app_dec(line, n, (unsigned long)pid);
    say_len(n);
}

/* --- the document's geometry ------------------------------------------ */

static unsigned line_start_of(unsigned p)
{
    while (p > 0 && doc[p - 1] != '\n')
        p--;
    return p;
}

static unsigned line_end_of(unsigned p)
{
    unsigned i = p;
    while (i < dlen && doc[i] != '\n')
        i++;
    return i;
}

static unsigned caret_line(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < dpos; i++)
        if (doc[i] == '\n')
            n++;
    return n;
}

static unsigned caret_col(void)
{
    return dpos - line_start_of(dpos);
}

/* Lines in the document: a line is what lies between two newlines, and a file
 * that ends with one has no empty line after it. The same rule /bin/view counts
 * by, so the two programs say one number about the same bytes rather than two
 * that a test would have to reconcile. */
static unsigned doc_lines(void)
{
    unsigned n = 1;

    if (dlen == 0)
        return 0;
    for (unsigned i = 0; i < dlen; i++)
        if (doc[i] == '\n')
            n++;
    return n;
}

/* Keep the caret inside the twelve rows the frame can show. The view follows
 * the cursor rather than the other way round because a document longer than a
 * screen is the normal case, and scrolling a window with the mouse is not
 * something a program can ask the compositor for yet. */
static void reveal_caret(void)
{
    unsigned l = caret_line();

    if (l < dtop)
        dtop = l;
    else if (l >= dtop + NOTE_BODY_ROWS)
        dtop = l - NOTE_BODY_ROWS + 1;
}

/* --- the frame --------------------------------------------------------
 * Built in one buffer and written in one syscall. A row is bounded by
 * NOTE_LINE_MAX so a 512-byte line cannot wrap into the status row, and the
 * whole buffer is bounded, which is what makes those caps facts rather than
 * hopes. */
static unsigned row_len;

static void f_begin(void)
{
    row_len = 0;
}

/* An ink change, in the two bytes the kernel's surface parser eats and the zero
 * cells it counts. Separate from f_addc for exactly that reason: a row whose
 * escapes were charged to its 56-cell budget would be a row that lost the ends
 * of long lines to the highlighter. */
static void f_esc(int a)
{
    if (fn + 2 >= NOTE_FRAME_MAX)
        return;
    frame[fn++] = (char)HL_ESC;
    frame[fn++] = (char)('0' + a);
}

static void f_addc(char c)
{
    if (row_len >= NOTE_LINE_MAX || fn + 1 >= NOTE_FRAME_MAX)
        return;
    frame[fn++] = c;
    row_len++;
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

static void f_end(void)
{
    if (fn + 1 < NOTE_FRAME_MAX)
        frame[fn++] = '\n';
}

/* What a byte of the document looks like. A NUL would cut the row short and
 * any other control byte has no glyph in the 5x7 font, so both show as the
 * blank they would be anyway; the file's own bytes are never touched, this is
 * only what a human can see. */
static char show_byte(char c)
{
    if (c == '\n')
        return ' ';
    if (c >= 32 || c == '\t')
        return c;
    return ' ';
}

/* One document line, with the caret drawn into it.
 *
 * `cur` carries the pen from cell to cell, so a run of the same ink costs no
 * bytes at all: an escape goes on the wire when the class *changes* and not on
 * every character, which is why a coloured frame is barely larger than the
 * plain one it replaces. It starts at -1 for each row rather than keeping the
 * previous row's value, because the kernel's pen does survive from one write to
 * the next — a row that inherited its first cell's colour from the row above
 * would be showing a colour the classifier never asked for.
 *
 * With `colour` off — any name that is not source — put_byte() is exactly what
 * this function used to do, byte for byte: the same keys, the same caret, and
 * not one escape on the wire. That is not a compatibility note for the .txt
 * tests, it is the proof that highlighting is paint and not structure. */
static int cur;

static void put_byte(unsigned p)
{
    if (colour) {
        int a = cls[p];
        if (a != cur) {
            f_esc(a);
            cur = a;
        }
    }
    f_addc(show_byte(doc[p]));
}

static void f_doc_row(unsigned start, unsigned end, int caret_here)
{
    unsigned p = start;

    cur = -1;
    f_begin();
    while (p < end && (caret_here == 0 || p < dpos))
        put_byte(p++);
    if (caret_here)
        f_addc('|');
    while (p < end)
        put_byte(p++);
    f_end();
}

static void draw(void)
{
    unsigned i, shown, ln;
    int caret_shown;

    /* Repaint means re-decide. The document changed between one frame and the
     * next — that is the only reason an editor repaints — so the classes are
     * recomputed over the whole buffer here rather than kept from the open: a
     * letter typed in front of `int` turns it into `oint`, and a highlighter
     * that still called it a type would be showing what the file used to be.
     * Two thousand bytes of byte-at-a-time scan per keypress is what a machine
     * with no cache to invalidate can afford, and the frame is written once. */
    if (colour)
        nkey = hl_scan(doc, dlen, cls);

    fn = 0;
    f_addc('\f');                     /* a screen, not another line, and not a
                                       * row either: the title goes on row 0 */

    /* Row 0: which file, and whether what is on screen is in it. A truncated
     * document says so here, in the row a person reads first, because the
     * alternative is a file that looks complete. */
    f_begin();
    f_add("NOTEPAD - ");
    f_add(path);
    if (truncated)
        f_add(dirty ? "  (TRONQUE MODIFIE)" : "  (TRONQUE)");
    else
        f_add(dirty ? "  (MODIFIE)" : "  (SAUVE)");
    f_end();

    f_begin();
    f_end();

    /* Walk to the first line the view shows. */
    for (ln = 0, i = 0; ln < dtop && i < dlen; ln++) {
        while (i < dlen && doc[i] != '\n')
            i++;
        if (i < dlen)
            i++;                      /* step over that newline */
    }

    shown = 0;
    caret_shown = 0;
    while (shown < NOTE_BODY_ROWS && i < dlen) {
        unsigned start = i;
        unsigned here;

        while (i < dlen && doc[i] != '\n')
            i++;
        here = (dpos >= start && dpos <= i);
        f_doc_row(start, i, (int)here);
        if (here)
            caret_shown = 1;
        shown++;
        if (i < dlen)
            i++;
    }

    /* A caret with no line left to sit on: an empty document, or the empty line
     * after a trailing newline. `dpos <= end` above already counts the end of a
     * real line as inside it, so the caret the last row drew is not owed a second
     * row - and drawing one anyway put a stray `|` under the very first character
     * typed into a one-line document. */
    if (!caret_shown && shown < NOTE_BODY_ROWS) {
        f_doc_row(dlen, dlen, 1);
        shown++;
    }

    while (shown < NOTE_BODY_ROWS) {   /* keep every frame exactly 17 rows */
        f_begin();
        f_end();
        shown++;
    }

    f_begin();
    f_end();

    f_begin();
    f_add("LN ");
    f_addn(caret_line() + 1);
    f_add(" COL ");
    f_addn(caret_col() + 1);
    f_add("  ");
    f_addn(dlen);
    f_add(" OCTETS");
    if (colour) {
        /* The same count the viewer's status row carries, from the same scan.
         * On a text file this row is byte for byte what it has always been. */
        f_add("  ");
        f_addn(nkey);
        f_add(" MOTS-CLES");
    }
    f_end();

    f_begin();
    f_add("CTRL+S ECRIRE  CTRL+Q QUITTER  CTRL+B/F/P/N DEPLACER");
    f_end();

    wc_write(STDOUT_FILENO, frame, fn);
}

/* --- editing ---------------------------------------------------------- */

static void insert_char(char c)
{
    if (dlen + 1 >= NOTE_DOC_MAX)
        return;                       /* full: the status row already says so */
    for (unsigned p = dlen; p > dpos; p--)
        doc[p] = doc[p - 1];
    doc[dpos++] = c;
    dlen++;
    dirty = 1;
    reveal_caret();
}

static void erase_at(unsigned p)
{
    if (p >= dlen)
        return;
    for (unsigned q = p; q + 1 < dlen; q++)
        doc[q] = doc[q + 1];
    dlen--;
    dirty = 1;
}

/* Up and down keep the column the caret had, clamped to the line it lands on:
 * the reason a cursor can be moved at all is that a document is a flat run of
 * bytes and a line is only where the newlines are. */
static void move_line(int down)
{
    unsigned start = line_start_of(dpos);
    unsigned col = dpos - start;

    if (down) {
        unsigned end = line_end_of(dpos);
        unsigned next, nend;

        if (end >= dlen)
            return;                   /* already the last line */
        next = end + 1;
        nend = line_end_of(next);
        dpos = next + (col < nend - next ? col : nend - next);
    } else {
        unsigned prev, pend;

        if (start == 0)
            return;                   /* already the first line */
        prev = line_start_of(start - 1);
        pend = line_end_of(prev);
        dpos = prev + (col < pend - prev ? col : pend - prev);
    }
    reveal_caret();
}

/* Moving the caret is not an edit: nothing becomes dirty, but the view still
 * has to follow or the caret walks off the twelve rows on screen. */
static void caret_to(unsigned p)
{
    dpos = p;
    reveal_caret();
}

static void obey(int c)
{
    if (c == K_LF || c == K_TAB || (c >= 32 && c < 127)) {
        insert_char((char)c);
        return;
    }
    switch (c) {
    case K_BS:
        if (dpos > 0) {
            erase_at(dpos - 1);
            dpos--;
            reveal_caret();
        }
        return;
    case K_DEL:
        erase_at(dpos);
        return;
    case K_LEFT:
        if (dpos > 0)
            caret_to(dpos - 1);
        return;
    case K_RIGHT:
        if (dpos < dlen)
            caret_to(dpos + 1);
        return;
    case K_HOME:
        caret_to(line_start_of(dpos));
        return;
    case K_END:
        caret_to(line_end_of(dpos));
        return;
    case K_UP:
        move_line(0);
        return;
    case K_DOWN:
        move_line(1);
        return;
    default:
        return;                       /* any other control byte: nothing at all */
    }
}

/* --- the file --------------------------------------------------------- */

/* Compose "/tmp/<name>", adding ".txt" only when the name carries no extension,
 * and refuse anything that is not a readable word.
 *
 * The dot is new and it is the whole difference between "a note" and "a file":
 * `note work.c` has to open the file /bin/copy just put in the store, under the
 * name the person typed, and not the /tmp/work.c.txt a name-forcing editor
 * would have invented. The rules stay tight for it — letters, digits and _ as
 * before, at most one '.', and never first or last — so `..`, `.c` and `a..b`
 * are refused rather than cleverly interpreted. The store is flat and the
 * archive is searched first, so nothing here could actually be fooled; a name
 * that cannot be a traversal is a name a person can read back, and this is the
 * program's own filter rather than the kernel's. */
static int build_path(const char *name)
{
    unsigned long l = wc_strlen(name);
    unsigned n = 0;
    int dots = 0;

    if (l == 0 || l > NOTE_NAME_MAX)
        return 0;

    for (const char *p = NOTE_DIR; *p != '\0'; p++)
        path[n++] = *p;
    for (unsigned long i = 0; i < l; i++) {
        char c = name[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '_';
        if (c == '.') {
            if (dots != 0 || i == 0 || i + 1 >= l)
                return 0;             /* one dot, with a word either side */
            dots = 1;
            ok = 1;
        }
        if (!ok)
            return 0;
        path[n++] = c;
    }
    if (dots == 0) {
        for (const char *p = NOTE_EXT; *p != '\0'; p++)
            path[n++] = *p;
    }
    if (n + 1 > NOTE_PATH_MAX)
        return 0;                     /* unreachable with the caps above, stated */
    path[n] = '\0';
    return 1;
}

/* Does this name hold source? Colour goes on the wire only then. A note about a
 * shopping list has no keywords in it, and painting every byte of one in the
 * default ink would put an escape in front of text that means nothing — and
 * would make the plain-text frames the .txt tests already depend on depend on
 * the highlighter as well. */
static int name_is_source(void)
{
    unsigned long l = wc_strlen(path);

    if (l < 2)
        return 0;
    if (path[l - 1] != 'c' && path[l - 1] != 'h')
        return 0;
    return path[l - 2] == '.';
}

/* Read the whole file into the document. Returns 0 when the editor should open
 * (empty or full) and 1 when the store answered something that is not "no such
 * file". */
static int load(long pid)
{
    long fd = wc_open(path);

    /* WC_* macros are already negative (wcabi.h), so the number the kernel
     * hands back for "no such file" is WC_ENOENT itself and not its negation —
     * the difference between opening an empty buffer to write a new note and
     * quitting with an error on a first-time filename. */
    if (fd == WC_ENOENT) {
        unsigned n = wc_app_str(line, 0, "note: no file at ");
        n = wc_app_str(line, n, path);
        n = wc_app_str(line, n, " yet, a new one");
        say_pid(n, pid);
        return 0;
    }
    if (fd < 0) {
        unsigned n = wc_app_str(line, 0, "note: cannot open ");
        n = wc_app_str(line, n, path);
        n = wc_app_str(line, n, ", error ");
        n = wc_app_signed(line, n, fd);
        say_pid(n, pid);
        return 1;
    }

    while (dlen + 1 < NOTE_DOC_MAX) {
        long got = wc_read(fd, doc + dlen, NOTE_DOC_MAX - 1 - dlen);
        if (got <= 0)
            break;
        dlen += (unsigned)got;
    }

    /* A buffer that filled up is not a file that ended. Read on and count what
     * is left: the difference between "loaded 2047 bytes" and "2047 of 2400" is
     * the difference between an editor and a lie, and it is the second number
     * that makes the refusal in save_file() mean something to whoever reads
     * this log afterwards. */
    unsigned long extra = 0;
    if (dlen + 1 >= NOTE_DOC_MAX) {
        long more;

        while ((more = wc_read(fd, probe, sizeof probe)) > 0)
            extra += (unsigned long)more;
        if (more < 0) {
            wc_close(fd);
            unsigned n = wc_app_str(line, 0, "note: read failed on ");
            n = wc_app_str(line, n, path);
            n = wc_app_str(line, n, ", error ");
            n = wc_app_signed(line, n, more);
            say_pid(n, pid);
            return 1;
        }
    }
    doc[dlen] = '\0';
    wc_close(fd);

    {
        unsigned n = wc_app_str(line, 0, "note: loaded ");
        n = wc_app_dec(line, n, dlen);
        n = wc_app_str(line, n, " bytes from ");
        n = wc_app_str(line, n, path);
        say_pid(n, pid);
    }
    if (extra > 0) {
        unsigned n;

        truncated = 1;
        n = wc_app_str(line, 0, "note: truncated ");
        n = wc_app_dec(line, n, dlen);
        n = wc_app_str(line, n, " of ");
        n = wc_app_dec(line, n, dlen + extra);
        n = wc_app_str(line, n, " bytes");
        say_pid(n, pid);
    }
    /* The document as it came out of the store, before anybody typed a key.
     * /bin/copy prints the same number for the same bytes from the other side
     * of the archive boundary, and a test can put the two next to each other. */
    {
        unsigned n = wc_app_str(line, 0, "note: digest 0x");
        n = wc_app_hex(line, n, wc_fnv1a(doc, dlen));
        say_pid(n, pid);
    }
    return 0;
}

static int save_file(long pid)
{
    long fd;
    long wrote;
    unsigned n;

    /* The one thing an editor must never do. Half a file written back over a
     * whole one is not a save with a bug in it, it is a deletion wearing the
     * face of success — and the store would report it as a clean write, because
     * it would be telling the truth about the bytes it was handed. So the
     * document that is not the whole file does not go into the file, and the
     * answer is said out loud. The caller keeps running: what is on screen is
     * still worth reading, and Ctrl+Q will still report the edits as unsaved. */
    if (truncated) {
        n = wc_app_str(line, 0, "note: refused to save a truncated document");
        say_pid(n, pid);
        return 0;
    }

    fd = wc_open_flags(path, WC_O_WRONLY | WC_O_CREAT | WC_O_TRUNC);

    if (fd < 0) {
        n = wc_app_str(line, 0, "note: open to write failed on ");
        n = wc_app_str(line, n, path);
        n = wc_app_str(line, n, ", error ");
        n = wc_app_signed(line, n, fd);
        say_pid(n, pid);
        return 1;
    }

    wrote = wc_write(fd, doc, dlen);
    wc_close(fd);

    if (wrote != (long)dlen) {
        n = wc_app_str(line, 0, "note: wrote ");
        n = wc_app_dec(line, n, (unsigned long)(wrote < 0 ? 0 : wrote));
        n = wc_app_str(line, n, " of ");
        n = wc_app_dec(line, n, dlen);
        n = wc_app_str(line, n, " bytes");
        say_pid(n, pid);
        return 1;
    }

    dirty = 0;
    /* The count here is what the store reported back rather than what the
     * program holds, so "saved" is the file's word and not the program's hope. */
    n = wc_app_str(line, 0, "note: saved ");
    n = wc_app_dec(line, n, dlen);
    n = wc_app_str(line, n, " bytes to ");
    n = wc_app_str(line, n, path);
    say_pid(n, pid);
    /* And the bytes behind the count. A length can be matched by the wrong
     * content; this number, printed by /bin/copy over the file read back from
     * another process, cannot be matched by a document that lost one. */
    {
        unsigned m = wc_app_str(line, 0, "note: digest 0x");
        m = wc_app_hex(line, m, wc_fnv1a(doc, dlen));
        say_pid(m, pid);
    }
    return 0;
}

void _start(int argc, char **argv)
{
    long me = wc_getpid();
    const char *name;
    int quit;

    name = argc >= 2 ? argv[1] : "note";
    if (!build_path(name)) {
        unsigned n = wc_app_str(line, 0, "note: bad name ");
        n = wc_app_bytes(line, n, name, wc_strnlen(name, 20));
        say_len(n);
        say("note: a name is letters digits or _ with one dot, up to 12");
        wc_exit(NOTE_EXIT_USAGE);
    }
    colour = name_is_source();

    if (load(me) != 0)
        wc_exit(NOTE_EXIT_USAGE);

    reveal_caret();
    draw();
    if (colour) {
        /* Said after the first frame, because the first frame is what computed
         * it: the count and the colours a test is about to grep came out of the
         * same scan of the same bytes. */
        unsigned n = wc_app_str(line, 0, "note: highlighted ");
        n = wc_app_dec(line, n, nkey);
        n = wc_app_str(line, n, " keywords in ");
        n = wc_app_dec(line, n, doc_lines());
        n = wc_app_str(line, n, " lines");
        say_pid(n, me);
    }
    {
        unsigned n = wc_app_str(line, 0, "note: editing ");
        n = wc_app_str(line, n, path);
        say_pid(n, me);
    }

    for (;;) {
        char chunk[32];
        long got = wc_read(STDIN_FILENO, chunk, sizeof chunk);

        if (got < 0) {
            unsigned n = wc_app_str(line, 0, "note: keyboard read failed ");
            n = wc_app_signed(line, n, got);
            say_pid(n, me);
            wc_exit(NOTE_EXIT_READ);
        }
        if (got == 0) {
            /* Zero bytes back from a blocked read means the window manager no
             * longer says this program is the one on top, so the keys belong to
             * somebody else now. Stop rather than take them back. */
            say_pid(wc_app_str(line, 0, "note: lost the keyboard"), me);
            wc_exit(NOTE_EXIT_LOST);
        }

        quit = 0;
        for (long i = 0; i < got; i++) {
            int c = (unsigned char)chunk[i];

            if (c == K_SAVE) {
                if (save_file(me) != 0)
                    wc_exit(NOTE_EXIT_SAVE);
                continue;
            }
            if (c == K_QUIT) {
                quit = 1;
                break;
            }
            obey(c);
        }

        if (quit) {
            if (dirty) {
                unsigned n = wc_app_str(line, 0, "note: quit with ");
                n = wc_app_dec(line, n, dlen);
                n = wc_app_str(line, n, " unsaved bytes");
                say_pid(n, me);
                wc_exit(NOTE_EXIT_DIRTY);
            }
            say_pid(wc_app_str(line, 0, "note: quit"), me);
            wc_exit(NOTE_EXIT_OK);
        }

        draw();
    }
}
