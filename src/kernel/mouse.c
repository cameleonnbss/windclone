/* WindClone — PS/2 mouse implementation.
 *
 * Same 8042 controller as the keyboard, second channel. The bring-up order
 * matters and is the whole trick:
 *
 *   0xA8        enable the aux (mouse) port in the controller
 *   0x20/0x60   read-modify-write the config byte: aux IRQ on, aux clock on
 *   0xD4 0xF6   set the device to defaults (100 Hz, 3-byte packets)
 *   0xD4 0xF4   enable data reporting
 *   pic_unmask(2) + pic_unmask(12)   cascade, then the mouse line
 *
 * Forget the cascade and IRQ12 never arrives, even though the mouse is
 * happily sending packets into the controller's output buffer.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stddef.h>

#include "mouse.h"
#include "idt.h"
#include "io.h"
#include "serial.h"
#include "gfx.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

#define STATUS_INPUT_FULL  0x02   /* don't write yet */
#define STATUS_OUTPUT_FULL 0x01   /* a byte is waiting to be read */

static int      cursor_x;
static int      cursor_y;
static int      left_down;
static volatile int dirty;

static uint8_t  packet[3];
static int      packet_len;
static int      initialised;

/* A byte the firmware (or the keyboard) left in the output buffer looks
 * exactly like the answer to our next command, so drain before asking. */
static void ps2_flush_input(void)
{
    for (int i = 0; i < 64; i++) {
        if ((inb(PS2_STATUS) & STATUS_OUTPUT_FULL) == 0)
            return;
        (void)inb(PS2_DATA);
    }
}

static void wait_input_clear(void)
{
    for (int i = 0; i < 1000000; i++)
        if ((inb(PS2_STATUS) & STATUS_INPUT_FULL) == 0)
            return;
}

static int wait_output_full(void)
{
    for (int i = 0; i < 1000000; i++)
        if (inb(PS2_STATUS) & STATUS_OUTPUT_FULL)
            return 0;
    return -1;
}

static void aux_write(uint8_t byte)
{
    wait_input_clear();
    outb(PS2_CMD, 0xD4);        /* "the next data byte is for the aux port" */
    wait_input_clear();
    outb(PS2_DATA, byte);
}

static void aux_read_ack(void)
{
    if (wait_output_full() == 0)
        (void)inb(PS2_DATA);    /* 0xFA, or the 0xAA self-test on some hosts */
}

int mouse_init(void)
{
    cursor_x = gfx_ready() ? gfx_width() / 2 : 0;
    cursor_y = gfx_ready() ? gfx_height() / 2 : 0;
    packet_len = 0;

    /* The whole handshake runs with interrupts off. A command answer is
     * just a byte in port 0x60: if IRQ1 fires mid-sequence, the keyboard
     * handler reads it and our wait for "the config byte" hangs until it
     * times out — which is exactly how this failed the first time. */
    __asm__ volatile("cli");

    ps2_flush_input();
    wait_input_clear();
    outb(PS2_CMD, 0xA8);        /* enable aux port */

    /* Read the live config byte and only add what we need: the same rule
     * the keyboard driver follows, for the same reason (bit layouts differ
     * between the QEMU model and real 8042s). */
    ps2_flush_input();
    wait_input_clear();
    outb(PS2_CMD, 0x20);
    if (wait_output_full() != 0) {
        serial_write("mouse: controller did not answer 0x20 (status ");
        serial_write_hex(inb(PS2_STATUS));
        serial_write(")\n");
        __asm__ volatile("sti");
        return -1;
    }
    uint8_t config = inb(PS2_DATA);
    config |= 0x02;             /* aux interrupt enabled */
    config &= (uint8_t)~0x20;   /* aux clock enabled    */

    wait_input_clear();
    outb(PS2_CMD, 0x60);
    wait_input_clear();
    outb(PS2_DATA, config);

    aux_write(0xF6);            /* set defaults: 100 Hz, 3-byte packets */
    aux_read_ack();
    aux_write(0xF4);            /* enable data reporting */
    aux_read_ack();

    pic_unmask(2);              /* cascade: slave IRQs never arrive without it */
    pic_unmask(12);             /* the mouse's own line */

    initialised = 1;
    serial_write("mouse: 8042 aux channel live (irq12 armed)\n");

    __asm__ volatile("sti");
    return 0;
}

void mouse_irq(void)
{
    if (!initialised)
        return;

    uint8_t status = inb(PS2_STATUS);
    if ((status & STATUS_OUTPUT_FULL) == 0)
        return;
    /* Bit 5 set means the byte in the buffer belongs to the aux channel.
     * A keyboard byte that slipped in here would desynchronise every
     * packet from then on, so it is dropped, not guessed at. */
    if ((status & 0x20) == 0)
        return;

    uint8_t byte = inb(PS2_DATA);
    packet[packet_len++] = byte;
    if (packet_len < 3)
        return;
    packet_len = 0;

    uint8_t flags = packet[0];

    /* The first byte of a packet always has bit 3 set; if it does not, we
     * are mid-stream and must resynchronise on the next one. */
    if ((flags & 0x08) == 0)
        return;

    int dx = (int)packet[1];
    int dy = (int)packet[2];

    /* Sign bits live in the flags byte, not in the data bytes. */
    if (flags & 0x10)
        dx |= ~0xFF;
    if (flags & 0x20)
        dy |= ~0xFF;

    /* Overflow means the delta is a lie, not a huge jump. */
    if (flags & 0x40) dx = 0;
    if (flags & 0x80) dy = 0;

    cursor_x += dx;
    cursor_y -= dy;             /* PS/2 y is up-positive */

    if (gfx_ready()) {
        if (cursor_x < 0) cursor_x = 0;
        if (cursor_y < 0) cursor_y = 0;
        if (cursor_x >= gfx_width())  cursor_x = gfx_width() - 1;
        if (cursor_y >= gfx_height()) cursor_y = gfx_height() - 1;
    }

    left_down = flags & 0x01;
    dirty = 1;

    static int announced;
    if (!announced) {
        announced = 1;
        serial_write("mouse: first packet decoded (dx/dy sign bits ok)\n");
    }
}

int mouse_take_dirty(void)
{
    if (!dirty)
        return 0;
    dirty = 0;
    return 1;
}

int mouse_x(void) { return cursor_x; }
int mouse_y(void) { return cursor_y; }
int mouse_left(void) { return left_down; }
