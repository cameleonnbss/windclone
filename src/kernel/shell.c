/* WindClone — command line: line editor, built-ins, and program launcher.
 *
 * Everything here runs from the idle loop, never from an IRQ: the keyboard
 * driver only queues bytes (see keyboard.c), so a `cat` that walks the initrd
 * or a program that enters ring 3 cannot deadlock an interrupt.
 *
 * The scrollback is a fixed grid of characters, not a framebuffer: the
 * window can be repainted (dragged, overlapped) as often as it likes and
 * the text is still there. That is the same reason a real terminal keeps
 * cells and not pixels.
 *
 * Most commands are no longer here. `ls`, `cat`, `echo`, `hello` are ELF
 * programs in /bin, launched with their arguments and given this grid as
 * their stdout — which is the whole reason the sink below exists: a program
 * that can only talk to the serial port is a program nobody can use.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stddef.h>

#include "shell.h"
#include "gfx.h"
#include "usermode.h"
#include "proc.h"
#include "vfs.h"
#include "keyboard.h"
#include "serial.h"
#include "string.h"
#include "timer.h"
#include "version.h"
#include "desktop.h"

#define SHELL_ROWS 17
#define SHELL_COLS 62
#define LINE_H     11

/* One scrollback grid, and the code that writes and paints it, lifted out of
 * the terminal so it can be shared. The terminal is one instance; every
 * launched program gets another (see `struct program` below). Text stays cells
 * rather than pixels for the reason a real terminal does: a window can be
 * dragged, overlapped and repainted as often as it likes and the scrollback is
 * still there. */
struct surface {
    char rows[SHELL_ROWS][SHELL_COLS + 1];
    int  next_row;                    /* where the next line goes        */
    char pending[SHELL_COLS + 1];     /* the line still being assembled  */
    int  pending_len;
    int  dirty;                       /* this surface changed since paint */
};

static struct surface term;           /* the shell's own window */

static char input[SHELL_COLS + 1];
static int  input_len;

/* --- surface helpers -------------------------------------------------- */

static void surf_clear(struct surface *s)
{
    for (int r = 0; r < SHELL_ROWS; r++)
        memset(s->rows[r], 0, sizeof s->rows[r]);
    s->next_row = 0;
    s->pending_len = 0;
    s->dirty = 1;
}

static void surf_scroll(struct surface *s)
{
    for (int r = 1; r < SHELL_ROWS; r++)
        memcpy(s->rows[r - 1], s->rows[r], SHELL_COLS + 1);
    memset(s->rows[SHELL_ROWS - 1], 0, SHELL_COLS + 1);
    s->next_row = SHELL_ROWS - 1;
}

/* The 5x7 font has no lowercase: fold on the way into the grid so the
 * scrollback is already in the form the screen will show. */
static char upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

static void surf_put_line(struct surface *s, const char *str)
{
    if (s->next_row >= SHELL_ROWS)
        surf_scroll(s);

    int c = 0;
    for (; str[c] != '\0' && c < SHELL_COLS; c++)
        s->rows[s->next_row][c] = upper(str[c]);
    s->rows[s->next_row][c] = '\0';
    s->next_row++;
    s->dirty = 1;
}

/* "TEXT" + decimal, without dragging printf in. */
static void surf_put_line_num(struct surface *s, const char *prefix,
                              uint64_t value)
{
    char buf[SHELL_COLS + 1];
    int n = 0;

    for (; prefix[n] != '\0' && n < SHELL_COLS - 20; n++)
        buf[n] = prefix[n];

    char digits[21];
    int d = 0;
    do {
        digits[d++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value != 0 && d < 20);
    while (d > 0 && n < SHELL_COLS)
        buf[n++] = digits[--d];

    buf[n] = '\0';
    surf_put_line(s, buf);
}

/* "EXIT -9" into a program's surface: signed, because a killed process is a
 * whole reason a window exists and a minus sign must survive to the screen. */
static void surf_put_exit(struct surface *s, int code)
{
    char buf[24];
    int n = 0;
    for (const char *p = "EXIT "; *p != '\0'; p++)
        buf[n++] = *p;

    unsigned u;
    if (code < 0) {
        buf[n++] = '-';
        u = (unsigned)(-code);
    } else {
        u = (unsigned)code;
    }
    char dig[12];
    int d = 0;
    do {
        dig[d++] = (char)('0' + (u % 10));
        u /= 10;
    } while (u != 0 && d < 12);
    while (d > 0)
        buf[n++] = dig[--d];

    buf[n] = '\0';
    surf_put_line(s, buf);
}

/* Paint a surface's rows at (x,y). Shared by the terminal and every program
 * window; neither knows the other's cells. */
static void surf_draw(const struct surface *s, int x, int y)
{
    for (int r = 0; r < SHELL_ROWS; r++)
        if (s->rows[r][0] != '\0')
            gfx_text(x, y + r * LINE_H, s->rows[r], gfx_rgb(0x22, 0xC5, 0x5E), 1);
}

/* The terminal's own line writer: every banner, echo, help and prompt line in
 * this file still calls these, they just route into the terminal surface. */
static void put_line(const char *str)
{
    surf_put_line(&term, str);
}

static void put_line_num(const char *prefix, uint64_t value)
{
    surf_put_line_num(&term, prefix, value);
}


/* --- commands --------------------------------------------------------- */

static void cmd_help(void)
{
    put_line("COMMANDES:");
    put_line("  HELP           CETTE LISTE");
    put_line("  ABOUT          VERSION ET UPTIME");
    put_line("  CLEAR          EFFACE L'ECRAN");
    put_line("  BAT <NOM>      EXECUTE /TMP/<NOM>.BAT");
    put_line("  - PROGRAMMES DANS /BIN -");
    put_line("  LS             LISTE L'INITRD");
    put_line("  CAT <FICHIER>  AFFICHE UN FICHIER");
    put_line("  ECHO <MOTS>    RENVOIE SES ARGUMENTS");
    put_line("  HELLO          UN PROGRAMME QUI PARLE EN RING 3");
    put_line("  MOTD           LIT /ETC/MOTD VIA LE VFS");
    put_line("  WHOAMI         MON PID - ON NE TUE PAS PID 1");
    put_line("  REAPER         ENFANT, KILL, WAIT - DANS CET ORDRE");
    put_line("  SAVE           ECRIT OU LIT UN FICHIER DANS /TMP");
    put_line("  NOTE <NOM>     EDITE /TMP/<NOM>.TXT");
    put_line("CE QUE TU LANCES ICI EST UN PROCESSUS: PID REEL,");
    /* The shell's pid is printed as the number the table holds, not as a "1"
     * written into this file: the claim on the screen is then whatever the
     * kernel actually did, and a boot that handed pid 1 to somebody else
     * shows up as a different number instead of a lie. */
    put_line_num("PARENT = LE SHELL, PID ", (uint64_t)proc_shell_pid());
}

/* --- a program's own window ------------------------------------------
 * A launched program gets a surface of its own, bound to the pid the shell was
 * handed. The console router below sends a *running* program's write(1) bytes
 * into that surface instead of the shared terminal, so the program truly owns
 * its window: the compositor paints it, the user drags / minimises / closes it,
 * and it stays on screen after the process is reaped — showing the exit code the
 * process left and its final output — until the user closes it. A program's own
 * children (a reaper's bell) were never launched by a click or a word, so their
 * output still lands in the terminal: that is the honest edge of "a launched
 * program gets a window", not "every process does".
 *
 * A bound pid is collision-proof because pids are never reused (proc.c's
 * next_pid only climbs), so a window can key itself to one forever. */
#define SHELL_PROG_MAX 6

struct program {
    int      used;           /* slot holds a window (live or finished)   */
    int      live;           /* 1 while its process still owns a CPU     */
    int      finished;       /* 1 once the process stopped               */
    int      pid;
    int      exit_code;
    uint64_t bytes;          /* output routed to this window (the claim) */
    int      routed_logged;  /* emit "wm: routed output" only once       */
    int      screen_logged;  /* emit "wm: screen cleared" only once. A program
                              * that repaints says it every keystroke, and a log
                              * line per frame would bury the one fact worth
                              * knowing: that \f reaches a window at all.        */
    char     name[PROC_NAME_MAX];
    struct surface s;
};

static struct program prog[SHELL_PROG_MAX];

static struct program *program_for_pid(int pid)
{
    if (pid <= 0)
        return NULL;
    for (int i = 0; i < SHELL_PROG_MAX; i++)
        if (prog[i].used && prog[i].live && prog[i].pid == pid)
            return &prog[i];
    return NULL;
}

/* Assemble a chunk of output into a surface's rows. Bytes arrive in whatever
 * sizes the program chose — half a line, a whole file, forty at a time — so a
 * pending row fills until a newline or a full line forces it out. */
static void surf_write(struct surface *s, const char *data, uint64_t len)
{
    for (uint64_t i = 0; i < len; i++) {
        char c = data[i];
        if (c == '\r')
            continue;
        if (c == '\f') {
            /* Form feed means "this is a screen, not another line". A program
             * that draws a whole frame — an editor, a viewer, a status panel —
             * would otherwise have to watch its own output scroll off a
             * scrollback it never asked for, one repaint at a time. Clearing on
             * \f is the smallest thing that turns an append-only surface into a
             * canvas, and it needs no syscall because the byte is already on
             * the wire the program is writing to. */
            surf_clear(s);
            continue;
        }
        if (c == '\n' || s->pending_len >= SHELL_COLS) {
            if (s->pending_len > 0) {
                s->pending[s->pending_len] = '\0';
                surf_put_line(s, s->pending);
                s->pending_len = 0;
            }
            if (c != '\n')
                s->pending[s->pending_len++] = c;   /* wrap: keep the byte */
            continue;
        }
        s->pending[s->pending_len++] = c;
    }
    s->dirty = 1;
}

/* The one console sink the kernel calls for a program's write(1)/write(2). It
 * decides by *whose* bytes these are: proc_current_pid() is that program while
 * it owns the CPU, so they go to its window; when the writer is the shell, a
 * boot demo, or an un-launched child, they go to the terminal instead. The
 * serial port has already received every byte before this is ever reached (see
 * vfs.c), so routing changes only what a human sees, never what CI greps. */
static void console_router(const char *data, uint64_t len)
{
    struct program *pr = program_for_pid(proc_current_pid());
    if (pr != NULL) {
        if (!pr->routed_logged) {
            pr->routed_logged = 1;
            serial_write("wm: routed output pid ");
            serial_write_dec((uint64_t)pr->pid);
            serial_write("\n");
        }
        pr->bytes += len;
        if (!pr->screen_logged) {
            for (uint64_t i = 0; i < len; i++) {
                if (data[i] != '\f')
                    continue;
                pr->screen_logged = 1;
                serial_write("wm: screen cleared pid ");
                serial_write_dec((uint64_t)pr->pid);
                serial_write("\n");
                break;
            }
        }
        surf_write(&pr->s, data, len);
        return;
    }
    surf_write(&term, data, len);
}

/* Claim a surface for a just-launched program. Returns its slot (which becomes
 * the window's content id) or -1 when all six hold windows nobody has closed
 * yet — in which case the launch still runs, its output just falls back to the
 * terminal rather than being silently dropped. */
int shell_program_open(const char *name, int pid)
{
    for (int i = 0; i < SHELL_PROG_MAX; i++) {
        struct program *pr = &prog[i];
        if (pr->used)
            continue;
        pr->used = 1;
        pr->live = 1;
        pr->finished = 0;
        pr->pid = pid;
        pr->exit_code = 0;
        pr->bytes = 0;
        pr->routed_logged = 0;
        pr->screen_logged = 0;
        int n = 0;
        for (; name[n] != '\0' && n + 1 < PROC_NAME_MAX; n++)
            pr->name[n] = name[n];
        pr->name[n] = '\0';
        surf_clear(&pr->s);
        pr->s.dirty = 0;          /* opening a window is not new output */
        return i;
    }
    return -1;
}

/* Once per desktop frame: a window bound to a pid that has stopped owning a CPU
 * is finished. Read the exit code by that never-reused pid, say so on the serial
 * line the tests grep, and leave the window open showing "EXIT n" under its
 * output. Closing it stays the user's decision, not the kernel's. */
void shell_program_pump(void)
{
    for (int i = 0; i < SHELL_PROG_MAX; i++) {
        struct program *pr = &prog[i];
        if (!pr->used || !pr->live)
            continue;
        if (proc_pid_live(pr->pid))
            continue;

        pr->live = 0;
        pr->finished = 1;
        int code = 0;
        if (proc_exit_status(pr->pid, &code) == 0)
            pr->exit_code = code;

        serial_write("wm: program window pid ");
        serial_write_dec((uint64_t)pr->pid);
        serial_write(" finished code ");
        serial_write_signed(pr->exit_code);
        serial_write(", ");
        serial_write_dec(pr->bytes);
        serial_write(" bytes\n");

        surf_put_exit(&pr->s, pr->exit_code);
    }
}

const char *shell_program_name(int slot)
{
    if (slot < 0 || slot >= SHELL_PROG_MAX || !prog[slot].used)
        return "";
    return prog[slot].name;
}

int shell_program_live(int slot)
{
    if (slot < 0 || slot >= SHELL_PROG_MAX || !prog[slot].used)
        return 0;
    return prog[slot].live;
}

int shell_program_exit_code(int slot)
{
    if (slot < 0 || slot >= SHELL_PROG_MAX || !prog[slot].used)
        return 0;
    return prog[slot].exit_code;
}

/* Any program surface changed since it was last painted — the reason a running
 * program's window repaints on its own, exactly as the terminal does. */
int shell_program_dirty_any(void)
{
    for (int i = 0; i < SHELL_PROG_MAX; i++)
        if (prog[i].used && prog[i].s.dirty)
            return 1;
    return 0;
}

/* Whether *this* program's surface changed since it was last painted. The
 * compositor uses it to damage only the window whose text moved, rather than
 * repainting the whole desktop because some program somewhere produced output.
 * 0 for a free or out-of-range slot, so a window bound to a stale slot is safe. */
int shell_program_dirty_slot(int slot)
{
    if (slot < 0 || slot >= SHELL_PROG_MAX || !prog[slot].used)
        return 0;
    return prog[slot].s.dirty;
}

void shell_program_clear_dirty_all(void)
{
    for (int i = 0; i < SHELL_PROG_MAX; i++)
        prog[i].s.dirty = 0;
}

void shell_program_draw(int slot, int x, int y, int width)
{
    (void)width;
    if (slot < 0 || slot >= SHELL_PROG_MAX || !prog[slot].used)
        return;
    surf_draw(&prog[slot].s, x, y);
}

/* The user closed the window. Free the slot so its six- window pool has room
 * again; the process it belonged to is already gone or already on its way, and
 * nothing here kills it — a window being dismissed is not a signal. */
void shell_program_close(int slot)
{
    if (slot < 0 || slot >= SHELL_PROG_MAX)
        return;
    prog[slot].used = 0;
    prog[slot].live = 0;
    prog[slot].finished = 0;
    prog[slot].pid = 0;
    prog[slot].exit_code = 0;
    prog[slot].bytes = 0;
    prog[slot].routed_logged = 0;
    prog[slot].name[0] = '\0';
    surf_clear(&prog[slot].s);
    prog[slot].s.dirty = 0;
}


/* --- running a program ----------------------------------------------- */
#define PROGRAM_PATH_MAX 64

/* The one path from a word to a running program. Both entry points — the
 * keyboard prompt and the desktop's start menu — come through here, so a
 * program started by a mouse click gets the same /bin lookup, the same
 * terminal as its stdout and the same exit code as one typed by hand. `verb`
 * and `origin` are the only difference, and they exist because a log that
 * cannot tell the two apart cannot prove which one a test drove.
 *
 * Everything that is not a built-in is looked for in /bin and started as a
 * *child of the shell's own process slot* (see proc.h), with the rest of the
 * line as argv and this grid as its console. It used to go through
 * usermode_run(), which has no process table: a program could not spawn, wait
 * or kill, because there was no entry for it to be anybody's parent or child.
 * Now `reaper` typed at the prompt does what the boot demo does — a real bell,
 * killed by a real kill, reaped by a real wait — from the keyboard or the
 * mouse. That is the difference between "the shell has commands" and "there is
 * an OS": the kernel never hears about a program's name beyond the path it
 * opened, and the program itself is a process like any other. */
static int launch(int argc, const char *const *argv,
                  const char *verb, const char *origin)
{
    char path[PROGRAM_PATH_MAX];
    int n = 0;

    for (const char *p = "/bin/"; *p != '\0'; p++)
        path[n++] = *p;
    for (int i = 0; argv[0][i] != '\0' && n < PROGRAM_PATH_MAX - 1; i++)
        path[n++] = argv[0][i];
    path[n] = '\0';

    if (!vfs_exists(path)) {
        put_line("COMMANDE INCONNUE. TAPE HELP.");
        serial_write("shell: no program ");
        serial_write(path);
        serial_write("\n");
        return -1;
    }

    /* The word the CI contract greps for: reaching this line with "running"
     * means the keyboard decoded the command in order through the real IRQ
     * path; with "launched" it means a mouse click reached the same code. */
    serial_write("shell: ");
    serial_write(verb);
    serial_write(" '");
    serial_write(argv[0]);
    serial_write("' ");
    serial_write(origin);
    serial_write("\n");

    int pid = 0;
    if (proc_spawn_async(path, argc, argv, &pid) == -1) {
        put_line("IMPOSSIBLE DE LANCER CE PROGRAMME");
        serial_write("shell: failed to run ");
        serial_write(path);
        serial_write("\n");
        return -1;
    }

    /* Honest about what just happened, and about what did not: this placed the
     * program in the table as the shell's child and returned while it is still
     * about to work. The old synchronous launch claimed a completion the instant
     * the program ended and froze the desktop until then; now launch() makes no
     * such claim. The exit code arrives later, on a visit that runs the process
     * to its end, through proc_reap_live()'s "ran and returned" / "exited with"
     * lines. And the program no longer shares the terminal's grid: it is given a
     * window of its own (a surface the console router streams its output into,
     * painted and driven by the desktop), so the compositor keeps painting
     * between slices *and* the program's text belongs to something the user can
     * move, overlap and — after it finishes — close. */
    int slot = shell_program_open(argv[0], pid);
    if (slot >= 0) {
        wm_open_program(slot, pid);
        serial_write("wm: program window opened '");
        serial_write(argv[0]);
        serial_write("' pid ");
        serial_write_dec((uint64_t)pid);
        serial_write("\n");
    } else {
        /* All six windows are still open on finished programs. The launch is not
         * refused for that — the process runs and its output falls back to the
         * terminal rather than vanishing, and the log says which happened. */
        serial_write("wm: no window room, '");
        serial_write(argv[0]);
        serial_write("' output to terminal\n");
    }

    serial_write("shell: ");
    serial_write(argv[0]);
    serial_write(" started, pid ");
    serial_write_dec((uint64_t)pid);
    serial_write(" (running)\n");
    return 0;
}

static void cmd_program(int argc, const char *const *argv, const char *origin)
{
    /* `origin` is the only difference between a keystroke and a script line,
     * and it is not cosmetic: a log that cannot tell the two apart cannot
     * prove which one a test drove. */
    launch(argc, argv, "running", origin);
}

/* The desktop's start menu calls this with one of the names it read out of
 * /bin. No arguments: a menu row is a program, not a command line. */
int shell_launch(const char *name)
{
    if (name == NULL || name[0] == '\0')
        return -1;

    /* The prompt echoes its own line in execute(); the menu has to write the
     * equivalent, or a program's output appears on screen with nothing saying
     * which program produced it. */
    char echo[SHELL_COLS + 1];
    int n = 0;
    for (const char *p = "DEMARRER : "; *p != '\0' && n < SHELL_COLS - 1; p++)
        echo[n++] = *p;
    for (int i = 0; name[i] != '\0' && n < SHELL_COLS - 1; i++)
        echo[n++] = name[i];
    echo[n] = '\0';
    put_line(echo);

    const char *argv[1];
    argv[0] = name;
    return launch(1, argv, "launched", "from the start menu");
}

static void cmd_about(void)
{
    put_line("WINDCLONE " WC_VERSION " - X86_64");
    put_line("KERNEL: C, ASM, SANS LIBC");
    put_line("RING 3 + INITRD + FRAMEBUFFER: OK");
    put_line_num("UPTIME: ", timer_ticks() / 100);
    put_line("SHELL: CE QUE TU TAPES EST LU PAR L'IRQ1");
}

static void cmd_bat(int argc, const char *const *argv);

/* One command line, wherever it came from. `line` is written in place — spaces
 * become NULs so argv[] points into it and there is no second copy to keep in
 * sync — and the buffer belongs to the caller, which is what lets a typed line
 * live in `input` and a script line live in `bat_line` while both go through
 * this one dispatcher. `origin` names which one it was. */
static void run_line(char *line, int len, const char *origin)
{
    const char *argv[USER_ARG_MAX];
    int argc = 0;
    int i = 0;

    while (i < len && argc < USER_ARG_MAX) {
        while (i < len && line[i] == ' ')
            line[i++] = '\0';
        if (i >= len)
            break;
        argv[argc++] = &line[i];
        while (i < len && line[i] != ' ')
            i++;
    }

    if (argc == 0)
        return;                       /* nothing but spaces */
    if (strcmp(argv[0], "help") == 0)
        cmd_help();
    else if (strcmp(argv[0], "about") == 0)
        cmd_about();
    else if (strcmp(argv[0], "clear") == 0)
        surf_clear(&term);
    else if (strcmp(argv[0], "bat") == 0)
        cmd_bat(argc, argv);
    else
        cmd_program(argc, argv, origin);
}

/* --- batch files: a script read out of the writable store -----------------
 * cmd.exe's oldest idea, and the reason v0.1.18 mattered: bytes a *program*
 * wrote are now something the shell can read back and obey. `bat demo` opens
 * /tmp/demo.bat, splits it on newlines and runs every line through the same
 * dispatcher a typed line goes through, so a script gets the built-ins and the
 * /bin programs exactly the way a person at the prompt does.
 *
 * Three limits, each one a refusal with a line in the log rather than a crash:
 *   - BAT_MAX_LINES commands, because a file that runs forever is not a script;
 *   - no `bat` inside a `bat`, so the depth is two and a self-referring file
 *     cannot eat the kernel stack it is executing on;
 *   - BAT_LINE_MAX bytes per line, and a longer line is truncated rather than
 *     skipped — a half command may fail, which is an answer, where quietly
 *     dropping it is a lie about what ran.
 * The whole file is read into a kernel static before the first line runs, so a
 * script cannot change underneath itself half way through (and on this store,
 * nothing else can change it either — there is no scheduler preemption of a
 * syscall that simple, but the rule is worth having anyway). */
#define BAT_NAME_MAX   12         /* same rule /bin/save uses: a word, not a path */
#define BAT_PATH_MAX   32         /* "/tmp/" + name + ".bat" + NUL, with room */
#define BAT_LINE_MAX   64         /* one command, SHELL_COLS minus the prompt */
#define BAT_MAX        512        /* the whole script, in one static */
#define BAT_MAX_LINES  16

static char bat_text[BAT_MAX];
static char bat_line[BAT_LINE_MAX];

/* Compose "/tmp/<name>.bat" into `out`. Returns the length, or 0 when the name
 * is not a word — letters, digits or underscore — because nothing that reaches
 * the VFS from here may look like a traversal. The store is flat and the
 * archive is searched first anyway, so no name here could have escaped; the
 * filter exists so a typo says "bad name" instead of making a file nobody
 * meant to make. */
static int bat_path(const char *name, char *out, int cap)
{
    const char *p = "/tmp/";
    int n = 0;

    if (name == NULL || name[0] == '\0' || cap < BAT_PATH_MAX)
        return 0;
    for (; *p != '\0'; p++)
        out[n++] = *p;
    for (int i = 0; name[i] != '\0'; i++) {
        char c = name[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '_';
        if (!ok || i >= BAT_NAME_MAX || n > cap - 6)
            return 0;
        out[n++] = c;
    }
    for (p = ".bat"; *p != '\0'; p++)
        out[n++] = *p;
    out[n] = '\0';
    return n;
}

static void cmd_bat(int argc, const char *const *argv)
{
    if (argc < 2) {
        put_line("USAGE: BAT <NOM>  EXECUTE /TMP/<NOM>.BAT");
        return;
    }

    char path[BAT_PATH_MAX];
    if (bat_path(argv[1], path, sizeof path) == 0) {
        put_line("NOM DE SCRIPT INVALIDE: DES LETTRES, CHIFFRES OU _.");
        serial_write("shell: bad batch name ");
        serial_write(argv[1]);
        serial_write("\n");
        return;
    }

    int64_t fd = vfs_open(path);
    if (fd < 0) {
        put_line("FICHIER DE SCRIPT INTROUVABLE.");
        serial_write("shell: no batch file ");
        serial_write(path);
        serial_write("\n");
        return;
    }

    int have = 0;
    while (have < BAT_MAX - 1) {
        int64_t got = vfs_read((int)fd, bat_text + have,
                               (uint64_t)(BAT_MAX - 1 - have));
        if (got <= 0)
            break;
        have += (int)got;
    }
    vfs_close((int)fd);
    bat_text[have] = '\0';

    serial_write("shell: batch file ");
    serial_write(path);
    serial_write(" (");
    serial_write_dec((uint64_t)have);
    serial_write(" bytes)\n");

    int ran = 0;
    int lines = 0;
    char *p = bat_text;

    while (*p != '\0') {
        int l = 0;
        while (*p != '\0' && *p != '\n') {
            if (*p != '\r' && l < BAT_LINE_MAX - 1)
                bat_line[l++] = *p;
            p++;
        }
        if (*p == '\n')
            p++;
        bat_line[l] = '\0';

        if (l == 0)
            continue;                 /* a blank line is not a command */

        if (lines >= BAT_MAX_LINES) {
            serial_write("shell: batch stopped at ");
            serial_write_dec((uint64_t)BAT_MAX_LINES);
            serial_write(" lines, more in ");
            serial_write(path);
            serial_write("\n");
            put_line("SCRIPT TROP LONG: LIMITE ATTEINTE.");
            break;
        }
        lines++;

        /* ';' is cmd.exe's comment character, and the line says so on the serial
         * port rather than vanishing: a transcript with holes in it is a
         * transcript you cannot trust. */
        if (bat_line[0] == ';') {
            serial_write("shell: batch comment skipped\n");
            continue;
        }

        char echo[BAT_LINE_MAX + 3];
        int e = 0;
        echo[e++] = '>';
        echo[e++] = ' ';
        for (int k = 0; k < l && e < BAT_LINE_MAX; k++)
            echo[e++] = bat_line[k];
        echo[e] = '\0';
        put_line(echo);

        /* No nesting. Checked on the raw line, before run_line() turns its
         * spaces into NULs, so this is a prefix test and not a token one. */
        if (strncmp(bat_line, "bat", 3) == 0 &&
            (bat_line[3] == '\0' || bat_line[3] == ' ')) {
            serial_write("shell: batch refused nested script\n");
            put_line("PAS DE SCRIPT DANS UN SCRIPT.");
            continue;
        }

        /* Spaces become NULs inside bat_line here, which is fine: this buffer
         * is rewritten from bat_text on the next iteration. */
        run_line(bat_line, l, "from a batch file");
        ran++;
    }

    serial_write("shell: batch ran ");
    serial_write_dec((uint64_t)ran);
    serial_write(" commands from ");
    serial_write(path);
    serial_write("\n");
}

static void execute(void)
{
    input[input_len] = '\0';

    /* Echo the prompt line into the scrollback first, so history reads
     * like a transcript instead of a series of answers. */
    char echo[SHELL_COLS + 3];
    int n = 0;
    echo[n++] = '>';
    echo[n++] = ' ';
    for (int i = 0; i < input_len && n < SHELL_COLS; i++)
        echo[n++] = input[i];
    echo[n] = '\0';
    put_line(echo);

    if (input_len != 0)
        run_line(input, input_len, "from the prompt");

    input_len = 0;
    input[0] = '\0';
    term.dirty = 1;
}

/* --- public ------------------------------------------------------------ */

void shell_init(void)
{
    surf_clear(&term);

    /* The shell takes its place in the process table before anything can be
     * typed, and the claim it makes is the one the rest of this file depends
     * on: a slot with a pid means the programs started here have a parent, so
     * they may spawn, wait and kill instead of being refused. It happens here,
     * in the shell's own bootstrap, rather than from kmain, because the slot
     * belongs to this subsystem and nothing else needs it. */
    proc_shell_start();

    /* The one console sink the kernel calls for a running program's write(1)/
     * write(2). It is a router, not a writer: a launched program's bytes go to
     * that program's own window, and everyone else's (the shell's own lines, the
     * boot demos, an un-launched child) go to this terminal. The serial port has
     * already received every byte before the router is ever reached — that is
     * what CI greps — so this changes only what a human at the screen sees. */
    vfs_set_console_sink(console_router);

    put_line("WINDCLONE - INVITE DE COMMANDES");
    put_line("TAPE HELP POUR LA LISTE DES COMMANDES.");
    put_line("");
    term.dirty = 1;
}

void shell_key(int c)
{
    if (c == '\n') {
        execute();
        return;
    }
    if (c == '\b') {
        if (input_len > 0)
            input[--input_len] = '\0';
        term.dirty = 1;
        return;
    }
    if (c >= 32 && c < 127 && input_len < SHELL_COLS - 1) {
        input[input_len++] = (char)c;
        input[input_len] = '\0';
        term.dirty = 1;
    }
}

void shell_draw(int x, int y, int width)
{
    (void)width;

    surf_draw(&term, x, y);

    /* Prompt + the line being typed, with a block cursor. Only the terminal
     * has these; a program's window is pure scrollback (shell_program_draw). */
    int py = y + SHELL_ROWS * LINE_H + 2;
    gfx_text(x, py, ">", gfx_rgb(0xEF, 0x44, 0x44), 1);
    gfx_text(x + 12, py, input, gfx_rgb(0xF8, 0xFA, 0xFC), 1);
    int cx = x + 12 + gfx_text_width(input, 1) + 2;
    gfx_fill(cx, py, 6, 8, gfx_rgb(0x22, 0xC5, 0x5E));
}

int shell_dirty(void)
{
    return term.dirty;
}

void shell_clear_dirty(void)
{
    term.dirty = 0;
}
