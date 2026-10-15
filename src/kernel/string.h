/* WindClone — freestanding string/memory helpers.
 * With -ffreestanding -fno-builtin there is no libc: the compiler may
 * still emit calls to memset/memcpy, so we must provide them ourselves.
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_STRING_H
#define WINDCLONE_STRING_H

#include <stddef.h>
#include <stdint.h>

void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int   memcmp(const void *a, const void *b, size_t n);

size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);

#endif
