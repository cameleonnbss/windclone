/* WindClone — ARP + ICMP over e1000. See net.h for the design notes.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stddef.h>

#include "net.h"
#include "e1000.h"
#include "timer.h"
#include "log.h"
#include "serial.h"

/* QEMU's SLIRP user-mode network hands the guest 10.0.2.15, answers for its
 * gateway at 10.0.2.2, and replies to an ICMP echo sent there. Those addresses
 * are static here; a real DHCP handshake is the next increment. The point of
 * this file is the receive path: ARP proves a frame can come back, and the ICMP
 * echo proves a full IPv4 packet - built, checksummed, answered - can too. */
static const uint8_t OUR_IP[4] = { 10, 0, 2, 15 };
static const uint8_t GW_IP[4]  = { 10, 0, 2, 2 };

enum { NET_OFF, NET_ARP, NET_PING, NET_DONE };

static int      state;
static uint8_t  our_mac[6];
static uint8_t  gw_mac[6];
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

void net_init(void)
{
    if (e1000_present() != 0) {
        serial_write("net: no e1000 - arp/icmp client stays offline\n");
        state = NET_OFF;
        return;
    }
    e1000_mac(our_mac);
    state = NET_ARP;
    tries = 0;
    seq = 0;
    last_send = 0;
    klog("net: arp/icmp client armed for 10.0.2.2 on top of e1000");
}

void net_visit(void)
{
    if (state != NET_ARP && state != NET_PING)
        return;

    uint8_t buf[256];

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
                state = NET_DONE;
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
        } else {
            if (tries == 0)
                klog("icmp: echo request to 10.0.2.2 (seq 0)");
            len = build_echo(buf, seq++);
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
