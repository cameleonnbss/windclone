/* WindClone — COM1 serial driver.
 * The serial port works identically under BIOS and UEFI, so it is our
 * ground-truth console for QEMU, real hardware, and CI smoke tests.
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_SERIAL_H
#define WINDCLONE_SERIAL_H

#include <stdint.h>
#include <stddef.h>

void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s);
void serial_write_len(const char *s, size_t n);
void serial_write_dec(uint64_t value);
void serial_write_hex(uint64_t value);

#endif
