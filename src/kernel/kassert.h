/* WindClone — kernel assertions.
 * kassert(x) panics with file + line when x is false. A failed assert is
 * a bug we want to *see*, so panic() also prints a stack backtrace.
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_KASSERT_H
#define WINDCLONE_KASSERT_H

void kassert_fail(const char *file, int line, const char *expr);
void panic(const char *why);           /* kernel.c: halt + backtrace */

#define kassert(expr)                                    \
    do {                                                 \
        if (!(expr))                                     \
            kassert_fail(__FILE__, __LINE__, #expr);     \
    } while (0)

#endif
