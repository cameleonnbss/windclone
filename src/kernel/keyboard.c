/* WindClone — PS/2 keyboard driver (Intel 8042 controller).
 *
 * Encoding decision taken during bring-up: we KEEP the 8042's AT
 * translation filter ON and decode its set-1-style make/break stream
 * (break = make | 0x80), instead of selecting a scan code set on the
 * device itself. The controller applies the translation when the host
 * READS the data port, so the stream is uniform no matter what set the
 * device speaks internally (set 2 at power on; OVMF never reprograms
 * it) and regardless of bytes the firmware queued before us. A first
 * attempt that forced "raw set 1" on the device while translation was
 * still on got double-mapped garbage from QEMU's model ('a' arrived
 * as 0x03, decoded as '2') — the bug that decided the design.
 *
 * Every decoded key is recorded in the Phase 3 log ring — not echoed
 * to serial per keystroke — because a typing user would drown the
 * console. The one exception is the FIRST key ever decoded: CI sends a
 * QEMU `sendkey` and greps for that marker to prove the whole
 * IRQ1 -> scancode -> layout -> ring path works end to end.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stddef.h>

#include "keyboard.h"
#include "io.h"
#include "idt.h"
#include "log.h"
#include "serial.h"

#define KBD_DATA      0x60  /* read: scancode / write: device command */
#define KBD_STATUS    0x64  /* read: status byte                       */
#define KBD_CMD       0x64  /* write: controller command               */
#define STATUS_OUT    0x01  /* output buffer full — data to read       */
#define STATUS_IN     0x02  /* input buffer full — controller busy     */
#define STATUS_AUX    0x20  /* last byte came from the mouse port      */

/* --- controller plumbing (bounded: we are pre-interrupt here) -------- */

static int kb_wait(uint8_t mask, uint8_t want)
{
    for (int i = 0; i < 1000000; i++) {
        uint8_t st = inb(KBD_STATUS);
        if ((st & mask) == want)
            return 1;
        io_wait();
    }
    return 0;
}

static void kb_cmd(uint8_t c)
{
    if (kb_wait(STATUS_IN, 0))
        outb(KBD_CMD, c);
}

static void kb_send(uint8_t b)
{
    if (kb_wait(STATUS_IN, 0))
        outb(KBD_DATA, b);
}

/* One bounded poll for a device response byte; 1 = got one. */
static int kb_read(uint8_t *b)
{
    if (!kb_wait(STATUS_OUT, STATUS_OUT))
        return 0;
    *b = inb(KBD_DATA);
    return 1;
}

/* --- scan code set 1 -> US layout ------------------------------------- */

#define MAP_SIZE 0x45   /* make codes we know stop at 0x44 (F10) */

static const char map_plain[MAP_SIZE] = {
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '-', [0x0D] = '=', [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1A] = '[', [0x1B] = ']', [0x1C] = '\n',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l',
    [0x27] = ';', [0x28] = '\'', [0x29] = '`', [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.', [0x35] = '/',
    [0x39] = ' ',
};

static const char map_shift[MAP_SIZE] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$', [0x06] = '%',
    [0x07] = '^', [0x08] = '&', [0x09] = '*', [0x0A] = '(', [0x0B] = ')',
    [0x0C] = '_', [0x0D] = '+',
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R', [0x14] = 'T',
    [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I', [0x18] = 'O', [0x19] = 'P',
    [0x1A] = '{', [0x1B] = '}',
    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F', [0x22] = 'G',
    [0x23] = 'H', [0x24] = 'J', [0x25] = 'K', [0x26] = 'L',
    [0x27] = ':', [0x28] = '"', [0x29] = '~', [0x2B] = '|',
    [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V', [0x30] = 'B',
    [0x31] = 'N', [0x32] = 'M', [0x33] = '<', [0x34] = '>', [0x35] = '?',
};

static int shift_down, caps_lock;
static int presses, releases;
static int first_key_reported;

/* E0/E1 are multi-byte prefixes (arrows, right ctrl, pause...). We do
 * not route those anywhere yet, so the state machine just swallows the
 * byte that follows so it is never mistaken for a make code. */
enum { K_NORMAL, K_E0, K_E1 } kb_state;

/* --- line-editor queue -------------------------------------------------
 * The IRQ handler must not call into the shell (it runs in interrupt
 * context and the shell allocates, prints and can even start a process),
 * so the two are joined by this ring: IRQ writes, the idle loop drains.
 * 64 bytes is far more than a human can type during one repaint.
 */
#define KB_QUEUE 64
static volatile char kb_queue[KB_QUEUE];
static volatile int  kb_head, kb_tail;

static void kb_enqueue(char c)
{
    int next = (kb_head + 1) % KB_QUEUE;
    if (next == kb_tail)
        return;                 /* full: drop the newest, never overwrite */
    kb_queue[kb_head] = c;
    kb_head = next;
}

int keyboard_take_char(void)
{
    if (kb_tail == kb_head)
        return -1;
    char c = kb_queue[kb_tail];
    kb_tail = (kb_tail + 1) % KB_QUEUE;
    return (unsigned char)c;
}

static void keyboard_decode(uint8_t sc)
{
    if (kb_state == K_E0) {
        kb_state = K_NORMAL;
        return;
    }
    if (kb_state == K_E1) {
        /* pause sequence: E1 14 77 E0 ... eat until something else */
        kb_state = (sc == 0x14) ? K_E1 : K_NORMAL;
        return;
    }
    if (sc == 0xE0) { kb_state = K_E0; return; }
    if (sc == 0xE1) { kb_state = K_E1; return; }
    if (sc == 0xF0 || sc == 0xAA || sc == 0x00)
        return; /* set-2 release prefix / BAT echo: not make codes */

    int release = (sc & 0x80) != 0;
    uint8_t base = sc & 0x7F;

    if (release) {
        releases++;
        if (base == 0x2A || base == 0x36)
            shift_down = 0;
    } else {
        presses++;
        if (base == 0x2A || base == 0x36)
            shift_down = 1;
        if (base == 0x3A)
            caps_lock = !caps_lock;
    }

    /* Report only printable makes; releases are proven by the counters
     * (presses must equal releases once a key has completed its cycle,
     * which the local-test sendkey check asserts out-of-band). */
    if (release || base >= MAP_SIZE)
        return;

    char c = shift_down ? map_shift[base] : map_plain[base];
    if (caps_lock && c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    /* Enter and Backspace are control characters but they are exactly what
     * a line editor needs, so they pass the printable-only filter. */
    if (c == 0 || (c < 32 && c != ' ' && c != '\n' && c != '\b'))
        return; /* modifiers and unknowns: counted, not printed */

    kb_enqueue(c);

    if (!first_key_reported) {
        first_key_reported = 1;
        serial_write("keyboard: key decoded end-to-end\n");
    }
    log_message("key: '%c' down (sc 0x%02x)", c, base);
}

/* --- IRQ1 -------------------------------------------------------------
 * Called with interrupts disabled (the stub already hid the PIC line).
 * Drain every queued byte: QEMU can coalesce press+release while the
 * CPU sat in a long hlt.
 */
void keyboard_irq(void)
{
    for (int guard = 0; guard < 32; guard++) {
        uint8_t st = inb(KBD_STATUS);
        if ((st & STATUS_OUT) == 0)
            break;
        uint8_t b = inb(KBD_DATA);
        if ((st & STATUS_AUX) == 0)
            keyboard_decode(b);
        /* mouse bytes (Phase 8 driver) are dropped for now */
    }
}

/* --- init -------------------------------------------------------------- */

void keyboard_init(void)
{
    uint8_t b;
    uint8_t mode = 0x41;    /* sane fallback: translate on + kbd IRQ on */

    /* Controller command byte. We deliberately KEEP the AT translation
     * filter ON instead of fighting it: while translated mode is on,
     * the 8042 converts whatever scan code set the device speaks
     * (set 2 is the PS/2 power-up default; OVMF never reprograms it)
     * into the raw set-1 make/break encoding — exactly what our layout
     * tables decode. It also applies the conversion at READ time, so
     * bytes the firmware queued before us come out clean too. That is
     * why no F0 set-selection commands are needed at all.
     * Bit-layout footnote (a real compatibility swamp): QEMU models the
     * original Intel 8042 layout (KBD_INT=0x01, DISABLE_KBD=0x10,
     * KCC=0x40) while 90s PS/2 hardware uses a different one (bit0
     * translate, bit1 IRQ1...). We therefore READ the live mode byte
     * from the controller and only OR safety bits onto it — a no-op on
     * any machine whose firmware configured the port correctly, and a
     * repair if it did not. Clearing 0x10 only ever helps: it is
     * "kbd interface disabled" on QEMU and "aux disabled" on PS/2
     * hardware (we have no aux device yet). */
    kb_cmd(0x20);
    if (kb_read(&b))
        mode = b;
    mode = (uint8_t)((mode & ~0x10u) | 0x41u);
    kb_cmd(0x60);
    kb_send(mode);

    kb_cmd(0xAE);               /* enable keyboard interface (not AD!) */

    /* Make sure scanning is on (BIOS/bootloader may have left it off
     * after reading via int 16h); F4 is harmless if it already is. */
    kb_send(0xF4);
    while (kb_read(&b))          /* swallow FA ack / BAT leftovers */
        ;

    serial_write("keyboard: 8042 live, translated set 1 (irq1 armed)\n");

    /* IRQ1 -> PIC vector 41; unmask AFTER the ports are sane. */
    pic_unmask(1);
    serial_write("step: keyboard live\n");
}

int keyboard_presses(void)
{
    return presses;
}

int keyboard_releases(void)
{
    return releases;
}
