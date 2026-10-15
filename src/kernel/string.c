/* WindClone — freestanding string/memory helpers implementation.
 * SPDX-License-Identifier: MIT
 */
#include "string.h"

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    while (n-- > 0)
        *d++ = (uint8_t)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n-- > 0)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d < s) {
        while (n-- > 0)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n-- > 0)
            *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    while (n-- > 0) {
        if (*x != *y)
            return (int)*x - (int)*y;
        x++;
        y++;
    }
    return 0;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p != '\0')
        p++;
    return (size_t)(p - s);
}

int strcmp(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n > 0) {
        if (*a != *b)
            return (int)(uint8_t)*a - (int)(uint8_t)*b;
        if (*a == '\0')
            return 0;
        a++;
        b++;
        n--;
    }
    return 0;
}
