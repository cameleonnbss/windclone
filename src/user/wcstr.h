/* WindClone — the handful of string helpers userland shares.
 *
 * Why a header and not a .c: the link is -nostdlib with no archive to pull
 * from, so a second object file would have to be added to every program's
 * rule in the Makefile. Four small static inline functions cost nothing, and
 * a program that uses only two of them keeps only those two.
 *
 * Why these are here at all: binutils will happily resolve a reference to
 * memcpy() out of libgcc when you build normally, and refuse it here. -nostdlib
 * means what it says, so a `struct wc_dirent` assignment or a strlen() would
 * break the build — which is the good outcome, because the bad one is a
 * fault in ring 3. Everything below is a loop over bytes, on purpose.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_WCSTR_H
#define WINDCLONE_WCSTR_H

#include "wcuser.h"

/* strlen, and its bounded cousin: a name coming back from getdirent lives in
 * a fixed 64-byte array that a malformed archive could fill without a NUL, so
 * the programs below never run an unbounded strlen() on it. */
static inline unsigned long wc_strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n] != '\0')
        n++;
    return n;
}

static inline unsigned long wc_strnlen(const char *s, unsigned long max)
{
    unsigned long n = 0;
    while (n < max && s[n] != '\0')
        n++;
    return n;
}

/* Exact match, no wildcards: cat only has to recognise "-" to know that it
 * means standard input rather than a file named "-". */
static inline int wc_streq(const char *a, const char *b)
{
    unsigned long i = 0;
    for (;;) {
        if (a[i] != b[i])
            return 0;
        if (a[i] == '\0')
            return 1;
        i++;
    }
}

/* Decimal, one byte at a time. There is no sprintf in userland and no
 * scratch buffer worth sharing between three programs for it. */
static inline void wc_put_dec(long fd, unsigned long value)
{
    char buf[24];
    unsigned i = 0;

    if (value == 0) {
        wc_write(fd, "0", 1);
        return;
    }
    while (value > 0 && i < sizeof buf) {
        buf[i++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (i > 0) {
        char c = buf[--i];
        wc_write(fd, &c, 1);
    }
}

#endif
