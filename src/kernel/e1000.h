/* WindClone — Intel 82540EM (e1000, PCI 8086:100e) Gigabit Ethernet driver.
 *
 * A polled driver, not an interrupt one: the desktop asks for a packet when it
 * wants one, and the answer is either there or it is not. That keeps this whole
 * file free of the PIC/IRQ plumbing a real driver would add, and it is honest —
 * QEMU's e1000 delivers whatever the SLIRP user-mode network gives us, and the
 * test harness runs a real BOOTP/DHCP exchange over it with no host network
 * access at all.
 *
 * Every buffer the card touches is exactly one physical frame: a descriptor
 * ring is 8 x 16 bytes and an RX buffer is one 4 KiB page (the card's default
 * 2048-byte receive buffer fits inside it), so each is contiguous by
 * construction and the DMA address is just the frame's own physical base. A
 * ring that straddled two non-adjacent frames would walk off into memory the
 * card was never given, and the card would cheerfully DMA into it.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_E1000_H
#define WINDCLONE_E1000_H

#include <stdint.h>

/* Find the card, map its BAR, reset it, and stand up the rings. Returns 0 on
 * success; safe to call once at boot after pci_init() and pmm_init(). */
int e1000_init(void);

/* 0 = the card is present and ready to send and receive; negative otherwise. */
int e1000_present(void);

/* The card's burned-in MAC address (six bytes), valid once present. */
void e1000_mac(uint8_t out[6]);

/* Put one Ethernet frame on the wire. Returns the length transmitted, or -1. */
int e1000_tx(const uint8_t *frame, int len);

/* Take one received frame into `out` (at most `maxlen` bytes). Returns the
 * length, or 0 when nothing had arrived. Never blocks. */
int e1000_rx(uint8_t *out, int maxlen);

#endif
