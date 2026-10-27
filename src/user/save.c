/* WindClone — /bin/save: the first program that puts bytes in a file.
 *
 * Every other program in /bin reads: `cat` copies a file out, `ls` lists the
 * archive, `motd` walks /etc/motd through the VFS. None of them could ever
 * *change* anything, because until v0.1.18 there was nowhere for a changed byte
 * to go — the initrd is a boot archive, and a write() that pretended to succeed
 * against it would have been a bug dressed as a feature.
 *
 * So this program is the proof of the other half, and it is deliberately shaped
 * around the one claim that matters: bytes written by one process have to be
 * readable by *another*. `save write demo hello there` starts a process, stores
 * the line, and dies. `save read demo` is a different process with a different
 * pid, no memory of the first one, and nothing but a path — and it prints those
 * bytes back. That is a file. Everything else about a filesystem (a disk,
 * directories, permissions, unlink) is still missing, and this file does not
 * claim otherwise: the store is RAM, so a reboot forgets it.
 *
 * Why the path is assembled rather than typed: the shell's command line is
 * tokenised on spaces, so a note's name here is letters, digits or underscore,
 * and the program puts the `/tmp/` prefix and the `.txt` extension around it.
 * Building a path from argv is also the first place a *program* has to think
 * about input: note_path() below is the whole filter, and it exists so that a
 * typo like `save read ../bin/ls` answers "bad name" instead of creating a file
 * whose name looks like a traversal. Nothing in the kernel would have been
 * fooled — the store is flat and the archive is always searched first — but a
 * name that cannot be a traversal is a name a human can read back.
 *
 * SPDX-License-Identifier: MIT */
#include "wcstr.h"

#define NOTE_DIR     "/tmp/"
#define NOTE_EXT     ".txt"
#define NOTE_NAME_MAX 12            /* letters, digits or _ */
#define NOTE_PATH_MAX 32            /* dir + name + ext + NUL, with room */
#define NOTE_CONTENT_MAX 512        /* one note */

static char path[NOTE_PATH_MAX];
static char content[NOTE_CONTENT_MAX];
static char line[64];
static char chunk[128];

/* Compose "/tmp/<name>.txt" into the static path. Returns 1 when the name is
 * usable and the result fits, 0 otherwise. */
static int note_path(const char *name)
{
    unsigned long l = wc_strlen(name);
    if (l == 0 || l > NOTE_NAME_MAX)
        return 0;

    unsigned n = 0;
    for (const char *p = NOTE_DIR; *p != '\0'; p++)
        path[n++] = *p;
    for (unsigned long i = 0; i < l; i++) {
        char c = name[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '_';
        if (!ok)
            return 0;               /* a name is a word, not a path */
        path[n++] = c;
    }
    for (const char *p = NOTE_EXT; *p != '\0'; p++)
        path[n++] = *p;
    if (n + 1 > NOTE_PATH_MAX)
        return 0;                   /* unreachable with the caps above, stated */
    path[n] = '\0';
    return 1;
}

/* One line, one write() — see wcstr.h: a tick may land between two syscalls and
 * tear a line across another process's output, and CI greps whole lines. */
static void say_len(unsigned n)
{
    line[n] = '\n';
    wc_write(STDOUT_FILENO, line, n + 1);
}

static void say(const char *s)
{
    say_len(wc_app_str(line, 0, s));
}

static void say_bad_name(const char *name)
{
    unsigned n = wc_app_str(line, 0, "save: bad name ");
    n = wc_app_bytes(line, n, name, wc_strnlen(name, 20));
    say_len(n);
    say("save: a name is letters, digits or _ , up to 12");
}

/* --- save write <name> <words...> --------------------------------------- */
static int do_write(int argc, char **argv)
{
    if (argc < 3) {
        say("save: usage save write <name> <words...>");
        return 1;
    }
    if (!note_path(argv[2])) {
        say_bad_name(argv[2]);
        return 1;
    }

    /* Join the remaining words with single spaces and end the note with a
     * newline, because a file whose last line has no terminator is the small
     * misery every editor has to work around. */
    unsigned n = 0;
    for (int i = 3; i < argc && n < NOTE_CONTENT_MAX; i++) {
        if (i > 3 && n < NOTE_CONTENT_MAX)
            content[n++] = ' ';
        unsigned long l = wc_strlen(argv[i]);
        for (unsigned long j = 0; j < l && n < NOTE_CONTENT_MAX; j++)
            content[n++] = argv[i][j];
    }
    if (n == 0) {
        say("save: nothing to write");
        return 1;
    }
    if (n < NOTE_CONTENT_MAX)
        content[n++] = '\n';
    /* A note that filled the buffer exactly has no terminator: the file is then
     * one full unbroken line, which is a long note, not a corrupt one. */

    long fd = wc_open_flags(path, WC_O_WRONLY | WC_O_CREAT | WC_O_TRUNC);
    if (fd < 0) {
        unsigned h = wc_app_str(line, 0, "save: open failed on ");
        h = wc_app_str(line, h, path);
        h = wc_app_str(line, h, ", error ");
        h = wc_app_signed(line, h, fd);
        say_len(h);
        return 1;
    }

    long wrote = wc_write(fd, content, n);
    long pid = wc_getpid();
    wc_close(fd);

    if (wrote != (long)n) {
        unsigned h = wc_app_str(line, 0, "save: wrote ");
        h = wc_app_dec(line, h, (unsigned long)(wrote < 0 ? 0 : wrote));
        h = wc_app_str(line, h, " of ");
        h = wc_app_dec(line, h, n);
        h = wc_app_str(line, h, " bytes ");
        h = wc_app_str(line, h, path);
        say_len(h);
        return 1;
    }

    unsigned h = wc_app_str(line, 0, "save: wrote ");
    h = wc_app_dec(line, h, n);
    h = wc_app_str(line, h, " bytes ");
    h = wc_app_str(line, h, path);
    h = wc_app_str(line, h, " pid ");
    h = wc_app_dec(line, h, (unsigned long)pid);
    say_len(h);
    return 0;
}

/* --- save read <name> ---------------------------------------------------- */
static int do_read(int argc, char **argv)
{
    if (argc < 3) {
        say("save: usage save read <name>");
        return 1;
    }
    if (!note_path(argv[2])) {
        say_bad_name(argv[2]);
        return 1;
    }

    /* Read-only on purpose: this is the half of the release that must not
     * change what it is reporting on. No argv word carries the note's text
     * either, so anything printed between the two lines below came out of the
     * store and nowhere else — which is the claim the headless test reads. */
    long fd = wc_open(path);
    if (fd < 0) {
        unsigned h = wc_app_str(line, 0, "save: cannot open ");
        h = wc_app_str(line, h, path);
        h = wc_app_str(line, h, ", error ");
        h = wc_app_signed(line, h, fd);
        say_len(h);
        return 1;
    }

    long pid = wc_getpid();
    unsigned h = wc_app_str(line, 0, "save: opening ");
    h = wc_app_str(line, h, path);
    h = wc_app_str(line, h, " pid ");
    h = wc_app_dec(line, h, (unsigned long)pid);
    say_len(h);

    unsigned long total = 0;
    for (;;) {
        long got = wc_read(fd, chunk, sizeof chunk);
        if (got == 0)
            break;                  /* clean end of file */
        if (got < 0) {
            wc_close(fd);
            h = wc_app_str(line, 0, "save: read failed on ");
            h = wc_app_str(line, h, path);
            h = wc_app_str(line, h, ", error ");
            h = wc_app_signed(line, h, got);
            say_len(h);
            return 1;
        }
        wc_write(STDOUT_FILENO, chunk, (unsigned long)got);
        total += (unsigned long)got;
    }
    wc_close(fd);

    h = wc_app_str(line, 0, "save: read back ");
    h = wc_app_dec(line, h, total);
    h = wc_app_str(line, h, " bytes ");
    h = wc_app_str(line, h, path);
    h = wc_app_str(line, h, " pid ");
    h = wc_app_dec(line, h, (unsigned long)pid);
    say_len(h);
    return 0;
}

/* --- save list ----------------------------------------------------------- */
static int do_list(void)
{
    unsigned shown = 0;

    for (unsigned long i = 0; ; i++) {
        struct wc_dirent de;
        if (wc_getdirent(i, &de) != 0)
            break;

        /* Only the store's own files: the archive is what `ls` is for, and this
         * program's subject is which names came out of a write(). */
        const char *p = "tmp/";
        unsigned k = 0;
        for (; p[k] != '\0'; k++)
            if (de.name[k] != p[k])
                break;
        if (p[k] != '\0')
            continue;

        unsigned n = wc_app_str(line, 0, "save: ");
        n = wc_app_bytes(line, n, de.name, wc_strnlen(de.name, WC_NAME_MAX - 1));
        n = wc_app_str(line, n, "  ");
        n = wc_app_dec(line, n, (unsigned long)de.size);
        say_len(n);
        shown++;
    }

    unsigned n = wc_app_str(line, 0, "save: listed ");
    n = wc_app_dec(line, n, shown);
    n = wc_app_str(line, n, " files in the store");
    say_len(n);
    return 0;
}

void _start(int argc, char **argv)
{
    int code;

    if (argc < 2) {
        say("save: usage save write|read|list <name> [words...]");
        wc_exit(1);
    }
    if (wc_streq(argv[1], "write"))
        code = do_write(argc, argv);
    else if (wc_streq(argv[1], "read"))
        code = do_read(argc, argv);
    else if (wc_streq(argv[1], "list"))
        code = do_list();
    else {
        say("save: unknown verb; try write, read or list");
        code = 1;
    }

    wc_exit(code);
}
