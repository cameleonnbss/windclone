/* WindClone — a minimal TCP client that fetches one page over HTTP.
 *
 * This is the top of the stack for now: on the IPv4/ICMP/UDP path the `net`
 * module already proved, it runs a three-way handshake (SYN, SYN/ACK, ACK)
 * against a real server, sends an HTTP GET, and reads the response status line
 * back. It is deliberately a client only, and deliberately simple — no
 * congestion control, no sliding window beyond stop-and-wait, no reassembly
 * across many segments. That is enough to prove the transport works end-to-end
 * against the emulator's stack, and it is the primitive a package manager
 * needs to pull a file over HTTP.
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
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_TCP_H
#define WINDCLONE_TCP_H

#include <stdint.h>

/* Start a fresh connection attempt. our_ip is the lease we negotiated, gw_mac
 * the gateway resolved by ARP; the server lives at 10.0.2.2 on SERVER_PORT. */
void tcp_init(const uint8_t *our_ip, const uint8_t *gw_mac);

/* Advance the TCP/HTTP state machine once: read any arrived segment, send the
 * next expected packet (or retransmit on the cadence). Non-blocking. Returns 1
 * once the exchange is finished - either the page was fetched or we gave up. */
int tcp_visit(void);

#endif
