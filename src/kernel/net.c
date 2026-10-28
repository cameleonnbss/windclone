/* WindClone — minimal ARP client over e1000. See net.h for the design notes.
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

/* QEMU's SLIRP user-mode network hands the guest 10.0.2.15 and answers for its
 * gateway at 10.0.2.2. We use those addresses statically here; a real DHCP
 * handshake is the next increment. The point of this file is the receive path,
 * not address policy. */
static const uint8_t OUR_IP[4] = { 10, 0, 2, 15 };
static const uint8_t GW_IP[4]  = { 10, 0, 2, 2 };

enum { NET_OFF, NET_WAIT, NET_DONE };

static int      state = NET_OFF;
static uint8_t  our_mac[6];
static uint8_t  gw_mac[6];
static uint64_t last_send;
static int      tries;

#define RETRY_TICKS 50u      /* half a second between requests  */
#define MAX_TRIES   20u      /* then give up: the emulator is not answering */

static void mac_str(const uint8_t m[6], char out[18])
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        out[i * 3]     = hex[m[i] >> 4];
        out[i * 3 + 1] = hex[m[i] & 0xF];
        out[i * 3 + 2] = (i == 5) ? '\0' : ':';
    }
}

/* Build a 42-byte ARP request ("who-has 10.0.2.2, tell 10.0.2.15"). The e1000
 * pads anything shorter than 64 bytes on transmit, so we send the frame exactly
 * as long as the protocol needs and let the card handle the Ethernet minimum. */
static int build_request(uint8_t *f)
{
    int i;
    for (i = 0; i < 6; i++)  f[i] = 0xFF;              /* dst: broadcast      */
    for (i = 0; i < 6; i++)  f[6 + i] = our_mac[i];    /* src: our MAC        */
    f[12] = 0x08; f[13] = 0x06;                        /* ethertype: ARP      */
    f[14] = 0x00; f[15] = 0x01;                        /* htype: Ethernet     */
    f[16] = 0x00; f[17] = 0x08;                        /* ptype: IPv4         */
    f[18] = 0x06;                                      /* hlen                */
    f[19] = 0x04;                                      /* plen                */
    f[20] = 0x00; f[21] = 0x01;                        /* oper: request       */
    for (i = 0; i < 6; i++)  f[22 + i] = our_mac[i];   /* sender hardware     */
    for (i = 0; i < 4; i++)  f[28 + i] = OUR_IP[i];    /* sender protocol     */
    for (i = 0; i < 6; i++)  f[32 + i] = 0x00;         /* target hardware (?) */
    for (i = 0; i < 4; i++)  f[38 + i] = GW_IP[i];     /* target protocol     */
    return 42;
}

/* Is this received frame an ARP reply whose target protocol address is ours?
 * If so, copy the sender hardware address into gw_mac. Offsets follow the
 * layout build_request() writes out. */
static int is_reply_for_us(const uint8_t *f, int n)
{
    if (n < 42)
        return 0;
    if (!(f[12] == 0x08 && f[13] == 0x06))             /* not ARP             */
        return 0;
    if (!(f[20] == 0x00 && f[21] == 0x02))             /* not a reply         */
        return 0;
    for (int i = 0; i < 4; i++)                        /* target proto == ours*/
        if (f[38 + i] != OUR_IP[i])
            return 0;
    for (int i = 0; i < 4; i++)                        /* sender proto == gw  */
        if (f[28 + i] != GW_IP[i])
            return 0;
    for (int i = 0; i < 6; i++)
        gw_mac[i] = f[22 + i];                         /* sender hardware     */
    return 1;
}

void net_init(void)
{
    if (e1000_present() != 0) {
        serial_write("net: no e1000 - arp client stays offline\n");
        state = NET_OFF;
        return;
    }
    e1000_mac(our_mac);
    state = NET_WAIT;
    tries = 0;
    last_send = 0;
    klog("net: arp client armed for 10.0.2.2 on top of e1000");
}

void net_visit(void)
{
    if (state != NET_WAIT)
        return;

    uint8_t buf[256];

    /* Take whatever has arrived. We drain a few per visit rather than one, so a
     * reply that queued behind unrelated frames still gets read promptly, but we
     * never loop forever: the point is to yield back to hlt for SLIRP's turn. */
    for (int drained = 0; drained < 4; drained++) {
        int n = e1000_rx(buf, (int)sizeof buf);
        if (n <= 0)
            break;
        if (is_reply_for_us(buf, n)) {
            char s[18];
            mac_str(gw_mac, s);
            klog("arp: 10.0.2.2 is at %s - gateway resolved", s);
            state = NET_DONE;
            return;
        }
    }

    uint64_t now = timer_ticks();
    if (tries == 0 || (tries < (int)MAX_TRIES && now >= last_send + RETRY_TICKS)) {
        int len = build_request(buf);
        if (tries == 0)
            klog("arp: who-has 10.0.2.2 (broadcast)");
        if (e1000_tx(buf, len) < 0) {
            klog("arp: request %d not sent - card refused the frame", tries + 1);
            tries = (int)MAX_TRIES;                    /* stop hammering a dead TX */
            return;
        }
        tries++;
        last_send = now;
    } else if (tries >= (int)MAX_TRIES) {
        serial_write("net: gateway 10.0.2.2 never answered - arp timed out\n");
        state = NET_OFF;
    }
}

int net_resolved(void)
{
    return state == NET_DONE ? 1 : 0;
}
