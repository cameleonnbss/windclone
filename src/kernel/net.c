/* WindClone — ARP + ICMP over e1000. See net.h for the design notes.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stddef.h>

#include "net.h"
#include "tcp.h"
#include "e1000.h"
#include "timer.h"
#include "log.h"
#include "serial.h"
#include "string.h"

/* QEMU's SLIRP user-mode network hands the guest 10.0.2.15, answers for its
 * gateway at 10.0.2.2, replies to an ICMP echo sent there, and runs a BOOTP/
 * DHCP server on 10.0.2.2. This file walks up the stack one honest rung at a
 * time: ARP proves a frame can come back, the ICMP echo proves a full IPv4
 * packet - built, checksummed, answered - can too, and the DHCP discover/offer
 * is the first UDP datagram, so the transport header and its pseudo-header
 * checksum get exercised against a real server that hands back a lease. */
static const uint8_t OUR_IP[4] = { 10, 0, 2, 15 };
static const uint8_t GW_IP[4]  = { 10, 0, 2, 2 };

/* A DHCP transaction id is echoed by the server, so matching it proves the
 * offer is the answer to OUR discover and not some other boot's leftover. */
#define DHCP_XID 0x0dc1a7e5u

enum { NET_OFF, NET_ARP, NET_PING, NET_DHCP, NET_TCP, NET_DONE };

/* A navigation the shell asked for, in the three states that matter to a test:
 * `req_busy` is set the moment net_start_fetch() accepts a request and cleared by
 * the pump when it takes it, so a fetch that was refused (no lease yet) can never
 * be mistaken for one that finished, and a second keystroke cannot put two
 * connections on top of one another. `req_pending` is the request itself waiting
 * for the pump to be free. */
static int      req_busy;
static int      req_pending;
static char     req_url[64];
static char     req_dest[64];

static int      state;
static uint8_t  our_mac[6];
static uint8_t  gw_mac[6];
static uint8_t  lease_ip[4];
static uint64_t last_send;
static int      tries;
static int      seq;

#define RETRY_TICKS 50u          /* half a second between requests     */
#define MAX_TRIES   40u          /* then give up: nothing is answering  */

static void mac_str(const uint8_t m[6], char out[18])
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        out[i * 3]     = hex[m[i] >> 4];
        out[i * 3 + 1] = hex[m[i] & 0xF];
        out[i * 3 + 2] = (i == 5) ? '\0' : ':';
    }
}

/* Big-endian stores: the wire is network byte order, and we write bytes rather
 * than casting structs so the layout is explicit and needs no padding worries. */
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | p[3];
}

/* Render a dotted-quad so the log can name the address the server offered. */
static void ip_str(const uint8_t *a, char out[16])
{
    int pos = 0;
    for (int i = 0; i < 4; i++) {
        int v = a[i];
        char dig[3], tmp[3];
        int t = 0, r = 0;
        if (v == 0) tmp[t++] = '0';
        while (v) { dig[r++] = (char)('0' + v % 10); v /= 10; }
        while (r) tmp[t++] = dig[--r];
        for (int j = 0; j < t; j++) out[pos++] = tmp[j];
        out[pos++] = (i == 3) ? '\0' : '.';
    }
}

/* The Internet checksum: one's complement of 16-bit words, one's complement
 * back down to 16 bits. Used for both the IPv4 header and the ICMP message. */
static uint16_t cksum(const uint8_t *p, int n)
{
    uint32_t sum = 0;
    for (int i = 0; i + 1 < n; i += 2)
        sum += ((uint32_t)p[i] << 8) | p[i + 1];
    if (n & 1)
        sum += (uint32_t)p[n - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)~sum;
}

/* Build a 42-byte ARP request: who-has 10.0.2.2, tell 10.0.2.15. The card pads
 * it to the 64-byte Ethernet minimum on transmit. */
static int build_who_has(uint8_t *f)
{
    for (int i = 0; i < 6; i++)  f[i] = 0xFF;              /* broadcast dst   */
    for (int i = 0; i < 6; i++)  f[6 + i] = our_mac[i];    /* our src         */
    f[12] = 0x08; f[13] = 0x06;                            /* ethertype ARP   */
    f[14] = 0x00; f[15] = 0x01;                            /* htype Ethernet  */
    f[16] = 0x00; f[17] = 0x08;                            /* ptype IPv4      */
    f[18] = 0x06;                                          /* hlen            */
    f[19] = 0x04;                                          /* plen            */
    f[20] = 0x00; f[21] = 0x01;                            /* oper request    */
    for (int i = 0; i < 6; i++)  f[22 + i] = our_mac[i];    /* sender hw       */
    for (int i = 0; i < 4; i++)  f[28 + i] = OUR_IP[i];     /* sender ip       */
    for (int i = 0; i < 6; i++)  f[32 + i] = 0x00;          /* target hw (?)   */
    for (int i = 0; i < 4; i++)  f[38 + i] = GW_IP[i];      /* target ip       */
    return 42;
}

/* Build an ICMP echo request inside an IPv4 packet inside an Ethernet frame,
 * addressed to the gateway we just resolved. Returns the frame length. */
static int build_echo(uint8_t *f, int s)
{
    for (int i = 0; i < 6; i++)  f[i] = gw_mac[i];         /* dst = gateway   */
    for (int i = 0; i < 6; i++)  f[6 + i] = our_mac[i];    /* src = us        */
    f[12] = 0x08; f[13] = 0x00;                            /* ethertype IPv4  */

    int ihl = 20, icmp_len = 8;                            /* no IP options,
                                                            * ICMP header only */
    int ip_total = ihl + icmp_len;
    f[14] = 0x45;                                          /* v4, ihl 20      */
    f[15] = 0x00;                                          /* tos             */
    put16(&f[16], (uint16_t)ip_total);                     /* total length    */
    put16(&f[18], 0x0000);                                 /* id              */
    put16(&f[20], 0x0000);                                 /* flags + frag    */
    f[22] = 64;                                            /* ttl             */
    f[23] = 0x01;                                          /* protocol ICMP   */
    put16(&f[24], 0x0000);                                 /* chk field, zero
                                                            * before checksum */
    for (int i = 0; i < 4; i++) { f[26 + i] = OUR_IP[i]; f[30 + i] = GW_IP[i]; }
    put16(&f[24], cksum(&f[14], ihl));                     /* header checksum  */

    f[34] = 0x08; f[35] = 0x00;                            /* type echo, code0 */
    put16(&f[36], 0x0000);                                 /* chk field, zero   */
    put16(&f[38], 0x00dc);                                 /* identifier        */
    put16(&f[40], (uint16_t)s);                            /* sequence          */
    put16(&f[36], cksum(&f[34], icmp_len));                /* ICMP checksum     */
    return 14 + ip_total;                                  /* = 42; card pads   */
}

/* An ARP reply for us? On a match, keep the sender's hardware address. */
static int arp_reply_for_us(const uint8_t *f, int n)
{
    if (n < 42 || !(f[12] == 0x08 && f[13] == 0x06))       /* not ARP        */
        return 0;
    if (!(f[20] == 0x00 && f[21] == 0x02))                 /* not a reply    */
        return 0;
    for (int i = 0; i < 4; i++)
        if (f[38 + i] != OUR_IP[i] || f[28 + i] != GW_IP[i])
            return 0;                                       /* wrong addrs    */
    for (int i = 0; i < 6; i++)
        gw_mac[i] = f[22 + i];                              /* sender hardware */
    return 1;
}

/* An ICMP echo reply from the gateway, addressed back to us? Reports the ttl
 * the answer came with so the log can show it is a live network hop. */
static int icmp_echo_reply(const uint8_t *f, int n, int *ttl_out)
{
    if (n < 42 || !(f[12] == 0x08 && f[13] == 0x00))       /* not IPv4       */
        return 0;
    if (f[23] != 0x01)                                      /* not ICMP       */
        return 0;
    for (int i = 0; i < 4; i++)
        if (f[26 + i] != GW_IP[i] || f[30 + i] != OUR_IP[i])
            return 0;                                       /* wrong addrs    */
    int ihl = (f[14] & 0x0F) * 4;                           /* header length  */
    if (n < 14 + ihl + 2)
        return 0;
    if (f[14 + ihl] != 0x00)                                /* not echo reply */
        return 0;
    *ttl_out = f[22];
    return 1;
}

/* The UDP checksum adds a pseudo-header - the source and destination addresses,
 * the protocol and the datagram length - to the same one's-complement sum used
 * for the IP header and ICMP. A wrong value here makes the emulator's stack drop
 * the datagram before it ever reaches its BOOTP server, so it is computed over
 * the exact bytes we send. */
static uint16_t udp_cksum(const uint8_t *ip, const uint8_t *udp, int udplen)
{
    uint32_t sum = 0;
    sum += ((uint32_t)ip[12] << 8) + ip[13];
    sum += ((uint32_t)ip[14] << 8) + ip[15];
    sum += ((uint32_t)ip[16] << 8) + ip[17];
    sum += ((uint32_t)ip[18] << 8) + ip[19];
    sum += 17u + (uint32_t)udplen;                 /* protocol + length     */
    for (int i = 0; i + 1 < udplen; i += 2)
        sum += ((uint32_t)udp[i] << 8) | udp[i + 1];
    if (udplen & 1)
        sum += (uint32_t)udp[udplen - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFFu) + (sum >> 16);
    uint16_t c = (uint16_t)~sum;
    return c ? c : 0xFFFF;                         /* zero means "unused"    */
}

/* Build a DHCPDISCOVER: Ethernet broadcast + IPv4 (0.0.0.0 -> 255.255.255.255,
 * proto 17) + UDP (client 68 -> server 67) + a BOOTP request carrying our MAC
 * and the DHCP options that ask for an address. Returns the frame length. */
static int build_dhcp_discover(uint8_t *f)
{
    const int OPTS = 17;
    const int dhcp_len = 240 + OPTS;               /* BOOTP fixed + options  */
    const int udp_len  = 8 + dhcp_len;
    const int ip_total = 20 + udp_len;

    for (int i = 0; i < 6; i++)  f[i] = 0xFF;      /* broadcast dst          */
    for (int i = 0; i < 6; i++)  f[6 + i] = our_mac[i];
    f[12] = 0x08; f[13] = 0x00;                    /* ethertype IPv4         */

    f[14] = 0x45; f[15] = 0x00;                    /* v4, ihl 20, tos        */
    put16(&f[16], (uint16_t)ip_total);
    put16(&f[18], 0x0000);                         /* id                     */
    put16(&f[20], 0x0000);                         /* flags + frag           */
    f[22] = 64; f[23] = 17;                        /* ttl, protocol UDP      */
    put16(&f[24], 0x0000);                         /* chk field, zero first   */
    for (int i = 0; i < 4; i++) { f[26 + i] = 0x00; f[30 + i] = 0xFF; }
    put16(&f[24], cksum(&f[14], 20));              /* IPv4 header checksum    */

    put16(&f[34], 68);                             /* src port: client        */
    put16(&f[36], 67);                             /* dst port: server        */
    put16(&f[38], (uint16_t)udp_len);
    put16(&f[40], 0x0000);                         /* chk field, zero first   */

    uint8_t *d = &f[42];                           /* BOOTP/DHCP payload      */
    for (int i = 0; i < 240; i++) d[i] = 0x00;
    d[0] = 1;                                      /* op: REQUEST            */
    d[1] = 1;                                      /* htype: Ethernet        */
    d[2] = 6;                                      /* hlen                   */
    put32(&d[4], DHCP_XID);                        /* transaction id         */
    put16(&d[10], 0x8000);                         /* broadcast flag         */
    for (int i = 0; i < 6; i++) d[28 + i] = our_mac[i];  /* chaddr           */
    d[236] = 0x63; d[237] = 0x82; d[238] = 0x53; d[239] = 0x63; /* cookie    */
    int o = 240;
    d[o++] = 53; d[o++] = 1; d[o++] = 1;           /* Message Type: DISCOVER */
    d[o++] = 61; d[o++] = 7; d[o++] = 1;           /* Client Identifier      */
    for (int i = 0; i < 6; i++) d[o++] = our_mac[i];
    d[o++] = 55; d[o++] = 2; d[o++] = 1; d[o++] = 3; /* Parameter Request    */
    d[o++] = 255;                                  /* End                    */

    put16(&f[40], udp_cksum(&f[14], &f[34], udp_len));
    return 14 + ip_total;
}

/* A DHCPOFFER answering our discover? Keys on UDP-to-client, BOOTREPLY, our
 * transaction id, the magic cookie, and the message-type option being OFFER (2);
 * on success reports the address (yiaddr) the server is holding out for us. */
static int dhcp_offer(const uint8_t *f, int n, uint8_t out_ip[4])
{
    if (n < 14 + 20 + 8 + 240) return 0;
    if (!(f[12] == 0x08 && f[13] == 0x00)) return 0;   /* IPv4      */
    if (f[23] != 17) return 0;                          /* UDP       */
    int ihl = (f[14] & 0x0F) * 4;
    const uint8_t *u = f + 14 + ihl;
    if ((int)(u - f) + 8 + 240 > n) return 0;
    if (((u[2] << 8) | u[3]) != 68) return 0;           /* to client */
    const uint8_t *d = u + 8;
    if (d[0] != 2) return 0;                            /* BOOTREPLY */
    if (get32(&d[4]) != DHCP_XID) return 0;            /* our xid   */
    if (!(d[236] == 0x63 && d[237] == 0x82 && d[238] == 0x53 && d[239] == 0x63))
        return 0;                                       /* cookie    */
    int dl = n - (int)(d - f);
    for (int i = 240; i + 1 < dl; ) {
        int opt = d[i];
        if (opt == 255) break;
        if (opt == 0) { i++; continue; }
        int ol = d[i + 1];
        if (opt == 53 && i + 2 < dl && d[i + 2] == 2) {  /* OFFER     */
            out_ip[0] = d[16]; out_ip[1] = d[17];
            out_ip[2] = d[18]; out_ip[3] = d[19];        /* yiaddr    */
            return 1;
        }
        i += 2 + ol;
    }
    return 0;
}

void net_init(void)
{
    if (e1000_present() != 0) {
        serial_write("net: no e1000 - arp/icmp/dhcp client stays offline\n");
        state = NET_OFF;
        return;
    }
    e1000_mac(our_mac);
    state = NET_ARP;
    tries = 0;
    seq = 0;
    last_send = 0;
    req_busy = 0;
    req_pending = 0;
    klog("net: arp/icmp/dhcp client armed for 10.0.2.2 on top of e1000");
}

/* A bounded copy of one half of a navigation request. The caller is the shell,
 * so the string is typed text: it is capped to the buffer rather than trusted,
 * and a request whose path will not fit is refused by the caller instead of being
 * sent half. */
static void copy_req(char *dst, int cap, const char *src)
{
    int i = 0;

    if (src == 0) {
        dst[0] = '\0';
        return;
    }
    while (src[i] != '\0' && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

int net_start_fetch(const char *url, const char *dest)
{
    /* Nothing to build a frame with until the lease arrived: the client has no
     * source address and no gateway MAC, and sending a SYN built from guesses
     * would be a connection that cannot be the one it claims to be. */
    if (state != NET_DONE || req_busy || req_pending)
        return 0;
    if (url == 0 || url[0] == '\0')
        return 0;
    if (strlen(url) >= sizeof req_url)
        return 0;                       /* refuse a path that would be sent half */
    copy_req(req_url, (int)sizeof req_url, url);
    copy_req(req_dest, (int)sizeof req_dest,
             (dest == 0 || dest[0] == '\0') ? "net/fetched" : dest);
    req_busy = 1;
    req_pending = 1;
    return 1;
}

int net_fetch_busy(void)
{
    return req_busy || req_pending;
}

const char *net_last_addr(void)
{
    return tcp_last_addr();
}

void net_visit(void)
{
    /* Once the lease is in hand the TCP/HTTP machine owns the pump: it drains
     * and retransmits on its own cadence, so hand it the loop and step back. */
    if (state == NET_TCP) {
        if (tcp_visit()) {
            state = NET_DONE;
            req_busy = 0;                 /* the machine is free to navigate again */
        }
        return;
    }

    /* The exchange is over and somebody asked for a page: take the request, empty
     * the ring of anything the last connection left in it, and hand the pump back.
     * Draining first matters - an ACK that arrives after our own FIN belongs to the
     * connection we just closed, and a new one must not start with it waiting. */
    if (state == NET_DONE && req_pending) {
        uint8_t drain[340];
        for (int i = 0; i < 8; i++)
            if (e1000_rx(drain, (int)sizeof drain) <= 0)
                break;
        req_pending = 0;
        tcp_start(lease_ip, gw_mac, req_url, req_dest);
        state = NET_TCP;
        return;
    }

    if (state != NET_ARP && state != NET_PING && state != NET_DHCP)
        return;

    uint8_t buf[340];

    /* Drain a few received frames, routing each by ethertype. We never loop
     * forever: the point is to hand the CPU back to hlt for SLIRP's next turn. */
    for (int drained = 0; drained < 4; drained++) {
        int n = e1000_rx(buf, (int)sizeof buf);
        if (n <= 0)
            break;
        if (state == NET_ARP && arp_reply_for_us(buf, n)) {
            char s[18];
            mac_str(gw_mac, s);
            klog("arp: 10.0.2.2 is at %s - gateway resolved", s);
            state = NET_PING;
            tries = 0;
            last_send = timer_ticks();
            return;
        }
        if (state == NET_PING) {
            int ttl = 0;
            if (icmp_echo_reply(buf, n, &ttl)) {
                klog("icmp: reply from 10.0.2.2: seq %d ttl %d - host reachable",
                     seq - 1, ttl);
                state = NET_DHCP;                           /* now ask for a lease */
                tries = 0;
                last_send = timer_ticks();
                return;
            }
        }
        if (state == NET_DHCP) {
            uint8_t ip[4];
            if (dhcp_offer(buf, n, ip)) {
                char s[16];
                ip_str(ip, s);
                klog("dhcp: offer %s from 10.0.2.2 - lease understood", s);
                for (int i = 0; i < 4; i++) lease_ip[i] = ip[i];
                tcp_start(lease_ip, gw_mac, "/", "net/fetched");
                state = NET_TCP;
                return;
            }
        }
    }

    uint64_t now = timer_ticks();
    if (tries >= (int)MAX_TRIES) {
        serial_write("net: 10.0.2.2 never answered - giving up\n");
        state = NET_OFF;
        return;
    }
    if (tries == 0 || now >= last_send + RETRY_TICKS) {
        int len;
        if (state == NET_ARP) {
            if (tries == 0)
                klog("arp: who-has 10.0.2.2 (broadcast)");
            len = build_who_has(buf);
        } else if (state == NET_PING) {
            if (tries == 0)
                klog("icmp: echo request to 10.0.2.2 (seq 0)");
            len = build_echo(buf, seq++);
        } else {
            if (tries == 0)
                klog("dhcp: discover broadcast");
            len = build_dhcp_discover(buf);
        }
        if (e1000_tx(buf, len) < 0) {
            klog("net: frame %d not sent - card refused it", tries + 1);
            tries = (int)MAX_TRIES;                         /* stop hammering dead TX */
            return;
        }
        tries++;
        last_send = now;
    }
}

int net_resolved(void)
{
    return state >= NET_PING ? 1 : 0;
}
