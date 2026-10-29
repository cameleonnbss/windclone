/* WindClone — a minimal TCP client that fetches one page over HTTP.
 *
 * This is the top of the stack for now: on the IPv4/ICMP/UDP path the `net`
 * module already proved, it runs a three-way handshake (SYN, SYN/ACK, ACK)
 * against a real server, sends an HTTP GET, reads the whole response back into
 * a buffer, and hands the body to the writable store as /net/fetched. That last
 * step is what turns "we read a status code" into a package manager's primitive:
 * a file pulled over HTTP that a program can then open like any other. It is
 * deliberately a client only, and deliberately simple - no congestion control,
 * no sliding window beyond stop-and-wait, a response capped at one buffer -
 * which is enough to pull a page end-to-end against the emulator's stack.
 *
 * Like the rest of the network code it is driven from the idle loop: SLIRP
 * only moves a connection forward while the guest yields the CPU, so nothing
 * here may block. tcp_visit() drains whatever arrived, advances the machine by
 * one honest step, and returns; the handshake and the GET retransmit on a slow
 * cadence the same way ARP, ICMP and DHCP do.
 *
 * The peer is a plain HTTP server started by the test harness on the host's
 * loopback, which SLIRP exposes to the guest at 10.0.2.2. We connect to the
 * gateway we already resolved, using the address the DHCP offer handed us.
 *
 * v0.1.31 added the part a browser needs: the request line and the destination
 * are arguments instead of constants, and every fetch gets its own ephemeral
 * port and its own initial sequence number, so a second navigation is a second
 * real connection rather than a replay of the first. The address of the last
 * fetch is kept for the address bar, which is the only way a program can show
 * what the kernel actually asked for.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_TCP_H
#define WINDCLONE_TCP_H

#include <stdint.h>

/* Start a fresh connection attempt. our_ip is the lease we negotiated, gw_mac
 * the gateway resolved by ARP; the server lives at 10.0.2.2 on SERVER_PORT.
 * `url` is the path asked for in the GET line and `dest` the name the body is
 * stored under; either may be NULL, which means the boot default - "/" and
 * /net/fetched. That is what lets the same machine answer a keystroke for a new
 * address and not only the one page it fetched at boot. */
void tcp_start(const uint8_t *our_ip, const uint8_t *gw_mac,
               const char *url, const char *dest);

/* The address the running or last finished fetch asked for, in the form the
 * address bar shows it (10.0.2.2:8080/second). NULL until a fetch has named
 * one. A shell built-in and /bin/browser read this so the text in the bar is
 * the kernel's own record of the request, not a string the program guessed. */
const char *tcp_last_addr(void);

/* Advance the TCP/HTTP state machine once: read any arrived segment, send the
 * next expected packet (or retransmit on the cadence). Non-blocking. Returns 1
 * once the exchange is finished - either the page was fetched or we gave up. */
int tcp_visit(void);

#endif
