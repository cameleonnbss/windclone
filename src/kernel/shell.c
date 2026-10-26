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

#define SHELL_ROWS 17
#define SHELL_COLS 62
#define LINE_H     11

static char rows[SHELL_ROWS][SHELL_COLS + 1];
static int  next_row;                 /* where the next line goes        */
static char input[SHELL_COLS + 1];
static int  input_len;
static int  dirty = 1;

/* --- grid helpers ----------------------------------------------------- */

static void grid_clear(void)
{
    for (int r = 0; r < SHELL_ROWS; r++)
        memset(rows[r], 0, sizeof rows[r]);
    next_row = 0;
}

static void scroll_up(void)
{
    for (int r = 1; r < SHELL_ROWS; r++)
        memcpy(rows[r - 1], rows[r], SHELL_COLS + 1);
    memset(rows[SHELL_ROWS - 1], 0, SHELL_COLS + 1);
    next_row = SHELL_ROWS - 1;
}

/* The 5x7 font has no lowercase: fold on the way into the grid so the
 * scrollback is already in the form the screen will show. */
static char upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

static void put_line(const char *s)
{
    if (next_row >= SHELL_ROWS)
        scroll_up();

    int c = 0;
    for (; s[c] != '\0' && c < SHELL_COLS; c++)
        rows[next_row][c] = upper(s[c]);
    rows[next_row][c] = '\0';
    next_row++;
    dirty = 1;
}

/* "TEXT" + decimal, without dragging printf into the shell. */
static void put_line_num(const char *prefix, uint64_t value)
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
    put_line(buf);
}

/* The same with a sign. A program the shell starts can now be killed - that
 * is what this release is - and a killed process exits with -SIGKILL, so the
 * number after "CODE DE SORTIE:" carries the difference between "it finished"
 * and "it was ended". Printing -9 as an unsigned value would not be a
 * slightly wrong number, it would be a different claim entirely. */
static void put_line_signed(const char *prefix, int value)
{
    char buf[SHELL_COLS + 2];
    int n = 0;

    for (; prefix[n] != '\0' && n < SHELL_COLS - 21; n++)
        buf[n] = prefix[n];
    if (value < 0)
        buf[n++] = '-';
    buf[n] = '\0';

    unsigned long magnitude = value < 0
        ? (unsigned long)(0LL - (long long)value)
        : (unsigned long)value;
    put_line_num(buf, (uint64_t)magnitude);
}

/* --- commands --------------------------------------------------------- */

static void cmd_help(void)
{
    put_line("COMMANDES:");
    put_line("  HELP           CETTE LISTE");
    put_line("  ABOUT          VERSION ET UPTIME");
    put_line("  CLEAR          EFFACE L'ECRAN");
    put_line("  - PROGRAMMES DANS /BIN -");
    put_line("  LS             LISTE L'INITRD");
    put_line("  CAT <FICHIER>  AFFICHE UN FICHIER");
    put_line("  ECHO <MOTS>    RENVOIE SES ARGUMENTS");
    put_line("  HELLO          UN PROGRAMME QUI PARLE EN RING 3");
    put_line("  MOTD           LIT /ETC/MOTD VIA LE VFS");
    put_line("  WHOAMI         MON PID - ON NE TUE PAS PID 1");
    put_line("  REAPER         ENFANT, KILL, WAIT - DANS CET ORDRE");
    put_line("CE QUE TU LANCES ICI EST UN PROCESSUS: PID REEL,");
    /* The shell's pid is printed as the number the table holds, not as a "1"
     * written into this file: the claim on the screen is then whatever the
     * kernel actually did, and a boot that handed pid 1 to somebody else
     * shows up as a different number instead of a lie. */
    put_line_num("PARENT = LE SHELL, PID ", (uint64_t)proc_shell_pid());
}

/* --- a program's stdout, wired into the grid -------------------------
 * The kernel hands write(1)/write(2) bytes here whenever a program runs.
 * They arrive in whatever chunks the program chose - half a line, a whole
 * file, forty bytes at a time - so there is a pending row that only gets
 * flushed on a newline, at the end of a line that filled up, or when the
 * program exits without one. */
static char pending[SHELL_COLS + 1];
static int  pending_len;

static void sink_put_pending(void)
{
    if (pending_len == 0)
        return;
    pending[pending_len] = '\0';
    put_line(pending);
    pending_len = 0;
}

static void shell_sink(const char *data, uint64_t len)
{
    for (uint64_t i = 0; i < len; i++) {
        char c = data[i];
        if (c == '\r')
            continue;
        if (c == '\n' || pending_len >= SHELL_COLS) {
            sink_put_pending();
            if (c != '\n')
                pending[pending_len++] = c;   /* wrap: keep the byte */
            continue;
        }
        pending[pending_len++] = c;
    }
    dirty = 1;
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

    int code = -1;
    int rc = proc_run_program(path, argc, argv, &code);
    if (rc == -1) {
        put_line("IMPOSSIBLE DE LANCER CE PROGRAMME");
        serial_write("shell: failed to run ");
        serial_write(path);
        serial_write("\n");
        return -1;
    }

    /* Whatever the program printed that had no trailing newline is still in
     * `pending` - a screen that lost a program's last words would look like
     * the program never said them. */
    sink_put_pending();

    if (rc == -2) {
        /* The scheduler came home with this program still un-exited - it was
         * blocked when nothing could wake it. There is no exit code to report,
         * and inventing one is the kind of claim this project does not make. */
        put_line("PROGRAMME JAMAIS REVENU (BLOQUE)");
        serial_write("shell: ");
        serial_write(argv[0]);
        serial_write(" never returned\n");
        return -1;
    }

    put_line_signed("CODE DE SORTIE: ", code);
    serial_write("shell: ");
    serial_write(argv[0]);
    serial_write(" ran and returned\n");
    serial_write("shell: ");
    serial_write(argv[0]);
    serial_write(" exited with ");
    serial_write_signed(code);
    serial_write("\n");
    return 0;
}

static void cmd_program(int argc, const char *const *argv)
{
    launch(argc, argv, "running", "from the prompt");
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

    if (input_len == 0) {
        /* bare Enter: nothing to do */
    } else {
        /* Tokenise in place: spaces become NULs, so argv[] points into
         * `input` and there is no second copy to keep in sync. The buffer is
         * a kernel static that outlives the call, and proc_create() copies
         * the strings onto the program's own stack before entering ring 3 - so
         * the program owns its own argv from the moment it starts, which is
         * what lets a shell reuse `input` for the next line. */
        const char *argv[USER_ARG_MAX];
        int argc = 0;
        int i = 0;

        while (i < input_len && argc < USER_ARG_MAX) {
            while (i < input_len && input[i] == ' ')
                input[i++] = '\0';
            if (i >= input_len)
                break;
            argv[argc++] = &input[i];
            while (i < input_len && input[i] != ' ')
                i++;
        }

        if (argc == 0)
            ;   /* nothing but spaces */
        else if (strcmp(argv[0], "help") == 0)
            cmd_help();
        else if (strcmp(argv[0], "about") == 0)
            cmd_about();
        else if (strcmp(argv[0], "clear") == 0)
            grid_clear();
        else
            cmd_program(argc, argv);
    }

    input_len = 0;
    input[0] = '\0';
    dirty = 1;
}

/* --- public ------------------------------------------------------------ */

void shell_init(void)
{
    grid_clear();

    /* The shell takes its place in the process table before anything can be
     * typed, and the claim it makes is the one the rest of this file depends
     * on: a slot with a pid means the programs started here have a parent, so
     * they may spawn, wait and kill instead of being refused. It happens here,
     * in the shell's own bootstrap, rather than from kmain, because the slot
     * belongs to this subsystem and nothing else needs it. */
    proc_shell_start();

    /* From here on, whatever a program writes to fd 1 or 2 is typed into this
     * grid. The serial port still receives every byte - that is what CI
     * greps - but a user at the screen only has this window. */
    vfs_set_console_sink(shell_sink);

    put_line("WINDCLONE - INVITE DE COMMANDES");
    put_line("TAPE HELP POUR LA LISTE DES COMMANDES.");
    put_line("");
    dirty = 1;
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
        dirty = 1;
        return;
    }
    if (c >= 32 && c < 127 && input_len < SHELL_COLS - 1) {
        input[input_len++] = (char)c;
        input[input_len] = '\0';
        dirty = 1;
    }
}

void shell_draw(int x, int y, int width)
{
    (void)width;

    for (int r = 0; r < SHELL_ROWS; r++)
        if (rows[r][0] != '\0')
            gfx_text(x, y + r * LINE_H, rows[r], gfx_rgb(0x22, 0xC5, 0x5E), 1);

    /* Prompt + the line being typed, with a block cursor. */
    int py = y + SHELL_ROWS * LINE_H + 2;
    gfx_text(x, py, ">", gfx_rgb(0xEF, 0x44, 0x44), 1);
    gfx_text(x + 12, py, input, gfx_rgb(0xF8, 0xFA, 0xFC), 1);
    int cx = x + 12 + gfx_text_width(input, 1) + 2;
    gfx_fill(cx, py, 6, 8, gfx_rgb(0x22, 0xC5, 0x5E));
}

int shell_dirty(void)
{
    return dirty;
}

void shell_clear_dirty(void)
{
    dirty = 0;
}
