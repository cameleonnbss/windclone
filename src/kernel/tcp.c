/* WindClone — minimal TCP client + one HTTP GET. See tcp.h for the design.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stddef.h>

#include "tcp.h"
#include "e1000.h"
#include "timer.h"
#include "log.h"
#include "serial.h"

#define SERVER_PORT   8080      /* the harness http.server on the host loopback */
#define CLIENT_PORT   12345     /* our fixed ephemeral port                     */
#define RETRY_TICKS   20u       /* ~200ms between retransmits                   */
#define MAX_TRIES     60u       /* then give up: the connection is not happening */

enum { TCPC_IDLE, TCPC_SYN_SENT, TCPC_ESTABLISHED, TCPC_SENT_GET,
       TCPC_DONE, TCPC_FAILED };

static int      tcp_state;
static uint8_t  our_ip[4];
static uint8_t  srv_ip[4] = { 10, 0, 2, 2 };
static uint8_t  our_mac[6];
static uint8_t  gw_mac[6];
static uint32_t iss;          /* our initial sequence number   */
static uint32_t snd_nxt;      /* next seq byte we send         */
static uint32_t rcv_nxt;      /* next seq byte we expect       */
static uint64_t last_send;
static int      tries;
static int      get_sent;
static int      bytes_read;

/* Big-endian primitives: the wire is network byte order. */
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | p[3];
}

/* The Internet checksum, fed by the IPv4 pseudo-header (both addresses, the
 * protocol and the segment length) and then the TCP header plus payload. A
 * wrong value here makes the peer's stack drop our segment silently. */
static uint16_t cksum_bytes(uint32_t seed, const uint8_t *p, int n)
{
    uint32_t sum = seed;
    for (int i = 0; i + 1 < n; i += 2)
        sum += ((uint32_t)p[i] << 8) | p[i + 1];
    if (n & 1)
        sum += (uint32_t)p[n - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)~sum;
}

/* Build one TCP segment inside IPv4 inside Ethernet. flags is the low byte of
 * the TCP control field (SYN/ACK/PSH/FIN bits). Payload may be NULL. Returns
 * the frame length. The checksum is filled once every byte exists. */
static int build(uint8_t *f, uint8_t flags, uint32_t seq, uint32_t ack,
                 const uint8_t *payload, int plen)
{
    int tcp_len = 20 + plen;
    int ip_total = 20 + tcp_len;

    for (int i = 0; i < 6; i++) f[i] = gw_mac[i];
    for (int i = 0; i < 6; i++) f[6 + i] = our_mac[i];
    f[12] = 0x08; f[13] = 0x00;                       /* ethertype IPv4      */

    f[14] = 0x45; f[15] = 0x00;                       /* v4, ihl 20, tos     */
    put16(&f[16], (uint16_t)ip_total);
    put16(&f[18], 0x0001);                            /* id                  */
    put16(&f[20], 0x4000);                            /* don't fragment      */
    f[22] = 64; f[23] = 0x06;                         /* ttl, protocol TCP   */
    put16(&f[24], 0x0000);
    for (int i = 0; i < 4; i++) { f[26 + i] = our_ip[i]; f[30 + i] = srv_ip[i]; }
    put16(&f[24], cksum_bytes(0, &f[14], 20));        /* IPv4 header checksum */

    uint8_t *t = &f[34];                               /* TCP header         */
    put16(&t[0], CLIENT_PORT);
    put16(&t[2], SERVER_PORT);
    put32(&t[4], seq);
    put32(&t[8], ack);
    t[12] = 0x50;                                      /* data offset 5      */
    t[13] = flags;
    put16(&t[14], 8000);                               /* window             */
    put16(&t[16], 0x0000);                             /* checksum slot      */
    put16(&t[18], 0x0000);                             /* urgent             */
    for (int i = 0; i < plen; i++) t[20 + i] = payload[i];

    /* TCP checksum = pseudo-header seed + the whole segment (header+payload). */
    uint32_t seed = 0;
    seed += ((uint32_t)our_ip[0] << 8) + our_ip[1];
    seed += ((uint32_t)our_ip[2] << 8) + our_ip[3];
    seed += ((uint32_t)srv_ip[0] << 8) + srv_ip[1];
    seed += ((uint32_t)srv_ip[2] << 8) + srv_ip[3];
    seed += 6u + (uint32_t)tcp_len;
    uint16_t cks = cksum_bytes(seed, t, tcp_len);
    put16(&t[16], cks ? cks : 0xFFFF);

    return 14 + ip_total;
}

/* Is this an IPv4/TCP segment carrying our connection? On a match, report the
 * flags, the sequence and ack numbers, and hand back a pointer to the payload
 * (and its length) so the caller can inspect the handshake or the HTTP body. */
static int tcp_segment(const uint8_t *f, int n, uint8_t *flags_out,
                       uint32_t *seq_out, uint32_t *ack_out,
                       const uint8_t **payload, int *plen_out)
{
    if (n < 14 + 20 + 20) return 0;
    if (!(f[12] == 0x08 && f[13] == 0x00)) return 0;   /* IPv4               */
    if (f[23] != 0x06) return 0;                        /* protocol TCP       */
    int ihl = (f[14] & 0x0F) * 4;
    const uint8_t *t = f + 14 + ihl;
    if ((int)(t - f) + 20 > n) return 0;
    if (get16(&t[0]) != SERVER_PORT) return 0;         /* from our server     */
    if (get16(&t[2]) != CLIENT_PORT) return 0;         /* to our port         */
    int tcp_len = (t[12] >> 4) * 4;
    int ip_total = get16(&f[16]);
    int avail = 14 + ip_total;
    if (avail > n) avail = n;                          /* card padded frame   */
    *flags_out = t[13];
    *seq_out = get32(&t[4]);
    *ack_out = get32(&t[8]);
    *plen_out = avail - (14 + ihl) - tcp_len;
    if (*plen_out < 0) *plen_out = 0;
    *payload = t + tcp_len;
    return 1;
}

void tcp_init(const uint8_t *ip, const uint8_t *mac)
{
    for (int i = 0; i < 4; i++) our_ip[i] = ip[i];
    for (int i = 0; i < 6; i++) gw_mac[i] = mac[i];
    e1000_mac(our_mac);
    iss = 0x13572468u;               /* a fixed ISS; the wire only needs it consistent */
    snd_nxt = iss;
    rcv_nxt = 0;
    tries = 0;
    get_sent = 0;
    bytes_read = 0;
    last_send = 0;
    tcp_state = TCPC_SYN_SENT;
    klog("tcp: connecting to 10.0.2.2:8080 from the leased address");
}

/* Send the single packet this state expects, retransmitting on the cadence. */
static void pump_send(uint8_t *f)
{
    uint64_t now = timer_ticks();
    if (tries >= (int)MAX_TRIES) {
        serial_write("tcp: 10.0.2.2:8080 never answered - giving up\n");
        tcp_state = TCPC_FAILED;
        return;
    }
    if (tries != 0 && now < last_send + RETRY_TICKS)
        return;

    if (tcp_state == TCPC_SYN_SENT) {
        if (tries == 0) klog("tcp: syn sent");
        int len = build(f, 0x02 /* SYN */, snd_nxt, 0, 0, 0);
        if (e1000_tx(f, len) < 0) { tcp_state = TCPC_FAILED; return; }
    } else if (tcp_state == TCPC_SENT_GET) {
        static const char req[] = "GET / HTTP/1.0\r\nHost: windclone\r\n\r\n";
        int len = build(f, 0x18 /* PSH|ACK */, snd_nxt, rcv_nxt,
                        (const uint8_t *)req, (int)sizeof(req) - 1);
        if (e1000_tx(f, len) < 0) { tcp_state = TCPC_FAILED; return; }
    } else {
        return;                       /* ESTABLISHED sends its GET inline      */
    }
    tries++;
    last_send = now;
}

int tcp_visit(void)
{
    if (tcp_state == TCPC_DONE || tcp_state == TCPC_FAILED || tcp_state == TCPC_IDLE)
        return 1;

    static uint8_t buf[600];

    /* Read what arrived and advance one honest step per match. */
    for (int drained = 0; drained < 4; drained++) {
        int n = e1000_rx(buf, (int)sizeof buf);
        if (n <= 0) break;
        uint8_t flags; uint32_t seq, ack; const uint8_t *pl; int plen;
        if (!tcp_segment(buf, n, &flags, &seq, &ack, &pl, &plen))
            continue;

        if (tcp_state == TCPC_SYN_SENT && (flags & 0x12) == 0x12 /* SYN|ACK */) {
            rcv_nxt = seq + 1;
            snd_nxt = iss + 1;
            tries = 0;
            tcp_state = TCPC_ESTABLISHED;
            klog("tcp: syn-ack received - connection established");
            /* fall through to send our ACK/GET on the next pump */
            break;
        }
        if (tcp_state == TCPC_SENT_GET && plen > 0) {
            bytes_read += plen;
            rcv_nxt = seq + (uint32_t)plen;
            /* Look for the status line "HTTP/1.x NNN" and read the 3 digits. */
            int status = 0;
            if (plen >= 12 && pl[0] == 'H' && pl[1] == 'T' && pl[2] == 'T' && pl[3] == 'P'
                && pl[4] == '/' && pl[5] == '1') {
                int k = 8;                             /* skip "HTTP/1.x " */
                while (k < plen && (pl[k] < '0' || pl[k] > '9')) k++;
                int d0 = pl[k], d1 = pl[k + 1], d2 = pl[k + 2];
                if (d0 >= '0' && d0 <= '9' && d1 >= '0' && d1 <= '9'
                    && d2 >= '0' && d2 <= '9')
                    status = (d0 - '0') * 100 + (d1 - '0') * 10 + (d2 - '0');
            }
            if (status >= 100 && status < 600) {
                klog("http: status %d, %d bytes read - first web fetch over tcp",
                     status, bytes_read);
                uint8_t f[84];
                build(f, 0x01 /* FIN */, snd_nxt, rcv_nxt, 0, 0);
                e1000_tx(f, 64);                       /* close politely     */
                tcp_state = TCPC_DONE;
                return 1;
            }
        }
    }

    if (tcp_state == TCPC_ESTABLISHED && !get_sent) {
        /* send the ACK+GET, then wait for the response */
        uint8_t f[128];
        static const char req[] = "GET / HTTP/1.0\r\nHost: windclone\r\n\r\n";
        int len = build(f, 0x18, snd_nxt, rcv_nxt, (const uint8_t *)req,
                        (int)sizeof(req) - 1);
        if (e1000_tx(f, len) < 0) { tcp_state = TCPC_FAILED; return 1; }
        get_sent = 1;
        tries = 0;
        last_send = timer_ticks();
        tcp_state = TCPC_SENT_GET;
        klog("http: GET sent, awaiting response");
        return 0;
    }

    pump_send(buf);
    return (tcp_state == TCPC_DONE || tcp_state == TCPC_FAILED) ? 1 : 0;
}
