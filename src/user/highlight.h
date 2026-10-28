/* WindClone — the lexer that /bin/view and /bin/note share.
 *
 * Why a header and not a .c: same reason as wcstr.h. Userland links -nostdlib
 * with no archive to pull symbols from, so a second object per program would
 * mean a second rule for all twenty names in USER_BINS. Small static inline
 * functions cost a program only what it calls.
 *
 * Why one lexer for two programs: because the claim this OS makes about its
 * editor is that what it colours is what the file is. Two lexers would drift —
 * a keyword added to the viewer and not to the editor is a bug no test can
 * catch, because each program would be right about its own table. One table,
 * one scan, and the viewer and the editor cannot disagree about what `int` is.
 *
 * Why the scan takes the buffer as an argument: view reads a file once and
 * never changes it, note mutates its document on every keystroke. A classifier
 * that reached for somebody's global `src[]` could only serve the first, and
 * note would have had to fake being a viewer to get colour. Taking (buf, len,
 * out) makes the note editor's repaint honest: it re-scans the whole document
 * for the frame it is about to write, so a token that stopped being a keyword
 * because of a letter typed in front of it loses its colour in the same frame.
 *
 * The rules are C's because C is the language this OS is written in and the
 * file it opens. What is deliberately not done: no nesting, no macro expansion,
 * no context. A token is what its bytes say in isolation, which is the same
 * rule a human highlighting by hand follows, and it is honest about its one
 * known lie — a word inside a comment is coloured as a comment, which is what a
 * reader wants and not what a compiler does.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_USER_HIGHLIGHT_H
#define WINDCLONE_USER_HIGHLIGHT_H

/* FS, the byte ASCII reserved for "file separator" and that no document text
 * has ever wanted. A program writes it followed by one digit and every glyph
 * after that is painted in that ink; the kernel's surface parser consumes the
 * pair, so it costs two bytes of wire and no cell of the screen. */
#define HL_ESC      0x1C

/* The palette, by role. These numbers are the indices in shell.c's color_ink[]
 * and nothing here can check that: the table is the kernel's, the meaning is
 * the program's, and the test that ties them together is that the colours
 * appear on the screen where this file says they should. Index 0 is the green
 * every cell starts in, so a program that never mentions a colour paints
 * exactly the screen it painted before colour existed. */
#define HL_TEXT     0      /* the default green: identifiers, punctuation */
#define HL_WHITE    1      /* emphasis: the file name in the title        */
#define HL_KEYWORD  2      /* if, for, return ...                         */
#define HL_STRING   3      /* "..." and '...'                             */
#define HL_NUMBER   4      /* 42, 0x1C, 8                                 */
#define HL_ERROR    5      /* a name that failed, an unmatched quote      */
#define HL_TYPE     6      /* int, char, void, struct ...                 */
#define HL_COMMENT  7      /* both kinds, and the line-number gutter      */

static inline int hl_is_word(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* Exact, case-sensitive, bounded by the token's own length: `w` points into the
 * caller's buffer and holds exactly n bytes, so a table entry longer than the
 * token cannot read past it. */
static inline int hl_matches(const char *s, const char *w, unsigned n)
{
    unsigned i = 0;

    while (i < n && s[i] != '\0') {
        if (s[i] != w[i])
            return 0;
        i++;
    }
    return i == n && s[i] == '\0';
}

static const char *const hl_keywords[] = {
    "if", "else", "while", "for", "do", "return", "break", "continue",
    "switch", "case", "default", "goto", "static", "const", "extern",
    "sizeof", "volatile", "inline", "define", "undef", "include", 0
};

static const char *const hl_types[] = {
    "int", "char", "void", "long", "short", "float", "double",
    "unsigned", "signed", "struct", "union", "enum", "typedef",
    "size_t", "uint8_t", "bool", 0
};

static inline int hl_word_class(const char *buf, unsigned start, unsigned n)
{
    for (int i = 0; hl_keywords[i] != 0; i++)
        if (hl_matches(hl_keywords[i], buf + start, n))
            return HL_KEYWORD;
    for (int i = 0; hl_types[i] != 0; i++)
        if (hl_matches(hl_types[i], buf + start, n))
            return HL_TYPE;
    return HL_TEXT;
}

static inline void hl_set(unsigned char *cls, unsigned start, unsigned end,
                          int a)
{
    for (unsigned i = start; i < end; i++)
        cls[i] = (unsigned char)a;
}

/* One pass over `len` bytes of `buf`, writing one palette index per byte into
 * `cls` (which the caller guarantees is at least `len` bytes). Returns how many
 * tokens it recognised as keywords — counted here, over the whole document, so
 * "highlighted N keywords" is a fact about the file and not about the twelve
 * rows currently on screen. */
static inline unsigned hl_scan(const char *buf, unsigned len,
                               unsigned char *cls)
{
    unsigned i = 0;
    unsigned nkey = 0;

    for (unsigned k = 0; k < len; k++)
        cls[k] = HL_TEXT;

    while (i < len) {
        char c = buf[i];

        if (c == '/' && i + 1 < len && buf[i + 1] == '/') {
            unsigned end = i;
            while (end < len && buf[end] != '\n')
                end++;
            hl_set(cls, i, end, HL_COMMENT);
            i = end;
            continue;
        }
        if (c == '/' && i + 1 < len && buf[i + 1] == '*') {
            unsigned end = i + 2;
            while (end + 1 < len && !(buf[end] == '*' && buf[end + 1] == '/'))
                end++;
            /* Unclosed: the comment runs to the end of the buffer. A block
             * comment that never terminates is a bug in the file, and the
             * honest response is to stop at the end rather than read past it
             * looking for a terminator that is not there. */
            end = end + 1 < len ? end + 2 : len;
            hl_set(cls, i, end, HL_COMMENT);
            i = end;
            continue;
        }
        if (c == '"' || c == '\'') {
            unsigned end = i + 1;
            while (end < len && buf[end] != c && buf[end] != '\n')
                end++;
            if (end < len && buf[end] == c) {
                hl_set(cls, i, end + 1, HL_STRING);
                i = end + 1;          /* include the closing quote */
                continue;
            }
            /* No close on this line: paint what there is in the error ink and
             * carry on, because a string that does not end is exactly the kind
             * of thing a highlighter should make visible instead of hiding. */
            hl_set(cls, i, end, HL_ERROR);
            i = end;
            continue;
        }
        if (c >= '0' && c <= '9') {
            unsigned end = i;
            while (end < len && hl_is_word(buf[end]))
                end++;
            hl_set(cls, i, end, HL_NUMBER);
            i = end;
            continue;
        }
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
            unsigned end = i;
            while (end < len && hl_is_word(buf[end]))
                end++;
            int a = hl_word_class(buf, i, end - i);
            if (a == HL_KEYWORD)
                nkey++;
            hl_set(cls, i, end, a);
            i = end;
            continue;
        }
        i++;
    }
    return nkey;
}

#endif
