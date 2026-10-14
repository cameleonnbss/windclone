/* WindClone — VGA text-mode driver implementation.
 * SPDX-License-Identifier: MIT
 */
#include "vga.h"

#include <stdint.h>
#include <stddef.h>

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

/* attribute: light grey on black */
#define VGA_ATTR 0x07

static uint16_t *buffer;          /* NULL until vga_init succeeds   */
static size_t column;
static size_t row;

void vga_init(void *hw)
{
    buffer = (uint16_t *)hw;
    column = 0;
    row = 0;
    if (buffer != NULL)
        kclear();
}

int vga_available(void)
{
    return buffer != NULL;
}

static void put_cell(char c, uint8_t attr)
{
    buffer[row * VGA_WIDTH + column] = (uint16_t)c | ((uint16_t)attr << 8);
}

static void scroll_up(void)
{
    for (size_t i = 0; i < (VGA_HEIGHT - 1) * VGA_WIDTH; i++)
        buffer[i] = buffer[i + VGA_WIDTH];
    for (size_t x = 0; x < VGA_WIDTH; x++)
        buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = ' ' | ((uint16_t)VGA_ATTR << 8);
    row = VGA_HEIGHT - 1;
}

static void put_char(char c)
{
    if (c == '\n') {
        column = 0;
        if (++row == VGA_HEIGHT)
            scroll_up();
        return;
    }
    if (c == '\r') {
        column = 0;
        return;
    }
    if (c == '\t') {
        column = (column + 8) & ~(size_t)7;
        if (column >= VGA_WIDTH) {
            column = 0;
            if (++row == VGA_HEIGHT)
                scroll_up();
        }
        return;
    }
    put_cell(c, VGA_ATTR);
    if (++column == VGA_WIDTH) {
        column = 0;
        if (++row == VGA_HEIGHT)
            scroll_up();
    }
}

void kprint(const char *s)
{
    if (buffer == NULL)
        return;
    while (*s != '\0')
        put_char(*s++);
}

void kclear(void)
{
    if (buffer == NULL)
        return;
    for (size_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        buffer[i] = ' ' | ((uint16_t)VGA_ATTR << 8);
    column = 0;
    row = 0;
}
