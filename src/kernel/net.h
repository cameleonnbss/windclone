/* WindClone — an ARP client, an ICMP pinger and a DHCP requester on the e1000.
 *
 * This module exists to answer one question the v0.1.25 release could not:
 * can the card *receive*? Sending was proven by a descriptor the card wrote
 * back, but a frame coming the other way needs the emulator's user-mode
 * network (SLIRP) to actually get CPU time, and that only happens when the
 * guest yields — so nothing here may busy-spin. The idle loop calls
 * net_visit() once per wake-up; it never blocks, it just takes whatever has
 * arrived and, on a timer, asks the question again.
 *
 * ARP is the smallest protocol that closes the loop: no checksums, and SLIRP
 * always answers a "who-has" for its gateway. We broadcast for 10.0.2.2, the
 * reply comes back through the receive ring, and parsing it is the honest
 * proof that a real packet landed in our memory via DMA. The MAC we log is
 * whatever the emulator answered — we assert that a well-formed reply for our
 * address arrived, not a specific number we hard-coded.
 *
 * Once the gateway is resolved the client moves on to ICMP: it builds a full
 * IPv4 header and an echo request, computes the Internet checksum over both,
 * sends them, and parses the reply. This is the first real IP round-trip — a
 * packet we constructed reaching the emulator's stack and an answer coming
 * back — and it is the ladder rung under DHCP and TCP, which arrive next.
 *
 * The last rung so far is DHCP: a UDP datagram (client port 68 to server 67)
 * with its pseudo-header checksum, a BOOTP DISCOVER carrying our MAC, and the
 * server's OFFER parsed back out. It is the first transport-layer exchange and
 * the first time the address is negotiated rather than assumed — the emulator's
 * BOOTP server hands us the lease, and we match on the transaction id so we
 * know the offer answers our discover.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_NET_H
#define WINDCLONE_NET_H

/* Stand the client up (it finds the e1000's MAC itself). Safe to call once at
 * boot right after e1000_init(). */
void net_init(void);

/* Drive the ARP state machine: take any received frames and, on a cadence,
 * (re)send the request. Non-blocking; call from the idle loop every iteration. */
void net_visit(void);

/* 1 once the gateway (10.0.2.2) MAC has been resolved from a real reply. */
int net_resolved(void);

#endif
