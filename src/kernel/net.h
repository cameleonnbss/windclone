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
 * v0.1.31 turned the boot-only exchange into a machine that can be asked again:
 * once the first page has been fetched the pump is idle, and net_start_fetch()
 * hands it back to the TCP client for a new address. That is what a browser's
 * address bar needs - not a second network stack, but the same one driven a
 * second time, with the lease and the resolved gateway it already negotiated.
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

/* Navigate: ask the machine for one more page, now, from the shell. `url` is
 * the path put in the GET line and `dest` the file the body lands in (either may
 * be NULL for the boot default). Returns 0 when the address was never resolved
 * and 1 when there is nothing to take the lease's place - the request is queued
 * and net_visit() drives it from the idle loop, because a ring-3 program cannot
 * pump SLIRP any more than it could before. Refuses a second navigation while one
 * is in flight rather than letting two connections share one pump. */
int net_start_fetch(const char *url, const char *dest);

/* 1 while a fetch the shell asked for is still running. */
int net_fetch_busy(void);

/* The address of the last fetch as the kernel recorded it, or NULL before the
 * first one. The address bar shows this, so the text is the machine's own answer
 * and not a copy of what someone typed. */
const char *net_last_addr(void);

#endif
