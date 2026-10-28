/* WindClone — Intel 82540EM (e1000) driver. See e1000.h for the design notes.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stddef.h>

#include "e1000.h"
#include "pci.h"
#include "pmm.h"
#include "paging.h"
#include "io.h"
#include "log.h"
#include "serial.h"

/* Register offsets, taken from the 82540EM programming reference (and matching
 * QEMU's own hw/net/e1000.c, which is the model these tests run against). */
#define R_CTRL    0x0000u
#define R_STATUS  0x0008u
#define R_EERD    0x0014u
#define R_ICR     0x00C0u
#define R_IMS     0x00D0u
#define R_IMC     0x00D8u
#define R_RCTL    0x0100u
#define R_TCTL    0x0400u
#define R_TIPG    0x0410u
#define R_RDBAL   0x2800u
#define R_RDBAH   0x2804u
#define R_RDLEN   0x2808u
#define R_RDH     0x2810u
#define R_RDT     0x2818u
#define R_TDBAL   0x3800u
#define R_TDBAH   0x3804u
#define R_TDLEN   0x3808u
#define R_TDH     0x3810u
#define R_TDT     0x3818u
#define R_MTA     0x5200u     /* 128 dwords of multicast filter  */
#define R_RA      0x5400u     /* RAL0 at +0, RAH0 at +4          */

#define CTRL_LRST   0x00000008u   /* link reset, self-clearing              */
#define CTRL_ASDE   0x00000020u   /* auto-speed detect enable               */
#define CTRL_SLU    0x00000040u   /* set link up                            */

#define RCTL_EN     0x00000002u   /* receiver enable                        */
#define RCTL_BAM    0x00008000u   /* broadcast accept mode (DHCP needs it)  */

#define TCTL_EN     0x00000002u   /* transmitter enable                     */
#define TCTL_PSP    0x00000008u   /* pad short packets to 64 bytes          */

#define DESC_DD     0x00000001u   /* descriptor done (both rings, status bit0) */
#define TXD_EOP     0x01000000u   /* end of packet                          */
#define TXD_IFCS    0x02000000u   /* insert FCS (Ethernet CRC)              */
#define TXD_RS      0x08000000u   /* report status (write DD back)          */

#define NIC_VIRT  0x11000000ULL   /* where BAR0 is mapped, clear of the FB  */
#define NIC_PAGES 64u             /* 256 KiB, enough for RA/MTA at 0x5400+  */
#define RING      8u              /* descriptors per ring                   */

/* Transmit descriptor: 16 bytes, one physical page holds RING of them. */
struct txd {
    uint64_t buf;      /* physical address of the data buffer */
    uint32_t lower;    /* length | cso<<16 | cmd<<24          */
    uint32_t upper;    /* status | css<<8 | special<<16       */
} __attribute__((packed));

/* Receive descriptor: 16 bytes. */
struct rxd {
    uint64_t buf;      /* physical address of the data buffer */
    uint16_t length;   /* bytes the card wrote                */
    uint16_t csum;
    uint8_t  status;   /* DD when the card has filled it      */
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

static volatile uint32_t *mmio;
static uint8_t  bdf[3];
static int      ready;
static uint8_t  mac[6];

static struct rxd  *rx_ring;                 /* HHDM virtual pointer        */
static uint64_t     rx_ring_phys;
static uint8_t     *rx_buf[RING];             /* one 4 KiB page each         */
static uint64_t     rx_buf_phys[RING];

static struct txd  *tx_ring;
static uint64_t     tx_ring_phys;
static uint8_t     *tx_buf;                   /* one 4 KiB page              */
static uint64_t     tx_buf_phys;

static unsigned rdh, rdt, tdt;

static inline uint32_t r32(uint32_t off) { return mmio[off / 4]; }
static inline void w32(uint32_t off, uint32_t v) { mmio[off / 4] = v; }

/* Flush the CPU caches around every DMA handoff. The card sees RAM, not our
 * cache: before it reads a frame we must push our writes out (wbinvd), and
 * before we read a descriptor or buffer it filled we must drop any stale line
 * (also wbinvd). This is the coarse version of what a real driver does with
 * cache-control instructions; it is correct, and a polled driver runs it only
 * at the ring edges, not in a hot loop. */
static inline void cache_sync(void)
{
    __asm__ volatile("wbinvd");
}

static void *mem(uint64_t phys) { return pmm_phys_to_virt(phys); }

static void mac_str(const uint8_t m[6], char out[18])
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        out[i * 3]     = hex[m[i] >> 4];
        out[i * 3 + 1] = hex[m[i] & 0xF];
        out[i * 3 + 2] = (i == 5) ? '\0' : ':';
    }
}

/* One contiguous frame, zeroed, reachable through the HHDM. Returns the
 * virtual pointer and writes the physical base into *phys_out, or NULL when the
 * machine is out of frames. */
static void *dma_page(uint64_t *phys_out)
{
    uint64_t phys = pmm_alloc_frame();
    if (!phys)
        return NULL;
    void *v = mem(phys);
    uint8_t *p = (uint8_t *)v;
    for (uint64_t i = 0; i < FRAME_SIZE; i++)
        p[i] = 0;
    *phys_out = phys;
    return v;
}

/* Transmit one harmless broadcast frame at the end of init and require the
 * card to report it done. This turns "the registers answer" into "the card
 * reads our memory and writes back to it" - the whole DMA contract every
 * later packet depends on. See the definition near e1000_tx(). */
static void tx_probe(void);

int e1000_init(void)
{
    if (!pci_find(0x8086, 0x100e, bdf)) {
        serial_write("e1000: no Intel 82540EM (8086:100e) on the PCI bus\n");
        return -1;
    }
    klog("e1000: found 82540EM at %02x:%02x.%x", bdf[0], bdf[1], bdf[2]);

    pci_enable_mmio_and_bus_master(bdf[0], bdf[1], bdf[2]);
    uint64_t bar = pci_bar(bdf[0], bdf[1], bdf[2], 0);
    if (bar == 0) {
        serial_write("e1000: BAR0 is not assigned - cannot map registers\n");
        return -1;
    }

    /* Map the register window uncached (PCD), exactly as the framebuffer is. */
    for (uint32_t i = 0; i < NIC_PAGES; i++)
        paging_map_page(NIC_VIRT + (uint64_t)i * 4096, bar + (uint64_t)i * 4096,
                        PTE_WRITE | PTE_PCD);
    mmio = (volatile uint32_t *)(uintptr_t)NIC_VIRT;

    /* A dead or unassigned BAR answers every read with 0xFFFFFFFF. Catch it
     * before writing anything to it. */
    if (r32(R_CTRL) == 0xFFFFFFFFu) {
        serial_write("e1000: MMIO reads all-ones - bad BAR mapping\n");
        mmio = NULL;
        return -1;
    }

    w32(R_IMC, 0xFFFFFFFFu);       /* polled driver: mask every interrupt off */
    w32(R_RCTL, 0);                /* stop the receiver before touching rings */
    w32(R_TCTL, 0);

    /* Link reset: set LRST, wait for the card to clear it itself. */
    w32(R_CTRL, r32(R_CTRL) | CTRL_LRST);
    for (int guard = 0; (r32(R_CTRL) & CTRL_LRST) && guard < 5000000; guard++)
        ;

    /* Burned-in MAC lives in RAL0/RAH0; QEMU seeds them from the netdev. */
    uint32_t ral = r32(R_RA), rah = r32(R_RA + 4);
    mac[0] = (uint8_t)(ral);       mac[1] = (uint8_t)(ral >> 8);
    mac[2] = (uint8_t)(ral >> 16); mac[3] = (uint8_t)(ral >> 24);
    mac[4] = (uint8_t)(rah);       mac[5] = (uint8_t)(rah >> 8);

    /* Clear the multicast filter to all-zero (accept none of it). */
    for (uint32_t i = 0; i < 128; i++)
        w32(R_MTA + i * 4, 0);

    /* Rings. Every buffer is a single page, so each is contiguous by
     * construction and its DMA address is its physical base. */
    rx_ring = (struct rxd *)dma_page(&rx_ring_phys);
    tx_ring = (struct txd *)dma_page(&tx_ring_phys);
    if (!rx_ring || !tx_ring) {
        serial_write("e1000: out of frames for descriptor rings\n");
        return -1;
    }
    for (unsigned i = 0; i < RING; i++) {
        rx_buf[i] = (uint8_t *)dma_page(&rx_buf_phys[i]);
        if (!rx_buf[i]) {
            serial_write("e1000: out of frames for rx buffers\n");
            return -1;
        }
        rx_ring[i].buf = rx_buf_phys[i];
        rx_ring[i].status = 0;
    }
    tx_buf = (uint8_t *)dma_page(&tx_buf_phys);
    if (!tx_buf) {
        serial_write("e1000: out of frames for tx buffer\n");
        return -1;
    }
    cache_sync();

    /* RX ring registers. */
    w32(R_RDBAL, (uint32_t)rx_ring_phys);
    w32(R_RDBAH, (uint32_t)(rx_ring_phys >> 32));
    w32(R_RDLEN, RING * sizeof(struct rxd));
    w32(R_RDH, 0);
    rdt = RING - 1;
    w32(R_RDT, rdt);
    w32(R_RCTL, RCTL_EN | RCTL_BAM);

    /* TX ring registers. */
    w32(R_TDBAL, (uint32_t)tx_ring_phys);
    w32(R_TDBAH, (uint32_t)(tx_ring_phys >> 32));
    w32(R_TDLEN, RING * sizeof(struct txd));
    w32(R_TDH, 0);
    tdt = 0;
    w32(R_TDT, tdt);
    w32(R_TCTL, TCTL_EN | TCTL_PSP | (0x0fu << 4) | (0x3fu << 12));
    w32(R_TIPG, 0x00640B12u);

    /* Force the link up with auto speed detection. */
    w32(R_CTRL, r32(R_CTRL) | CTRL_SLU | CTRL_ASDE);

    rdh = 0;
    ready = 1;

    char s[18];
    mac_str(mac, s);
    klog("e1000: link up, mac %s", s);
    klog("e1000: rx ring %u desc @0x%x, tx ring %u desc @0x%x",
         RING, (unsigned)rx_ring_phys, RING, (unsigned)tx_ring_phys);

    tx_probe();
    return 0;
}

int e1000_present(void) { return ready ? 0 : -1; }

void e1000_mac(uint8_t out[6])
{
    for (int i = 0; i < 6; i++)
        out[i] = mac[i];
}

int e1000_tx(const uint8_t *frame, int len)
{
    if (!ready || len <= 0 || (unsigned)len > FRAME_SIZE)
        return -1;

    unsigned i = tdt;
    for (int o = 0; o < len; o++)
        tx_buf[o] = frame[o];
    tx_ring[i].buf = tx_buf_phys;
    tx_ring[i].upper = 0;                       /* clear DD before sending    */
    tx_ring[i].lower = (uint32_t)len | TXD_EOP | TXD_IFCS | TXD_RS;
    cache_sync();                               /* push the frame out to RAM  */

    tdt = (i + 1) % RING;
    w32(R_TDT, tdt);                            /* ring the doorbell          */

    /* The card marks the descriptor done in RAM, not in our cache: drop the
     * stale line before every look so a writeback that already landed is
     * actually seen. A real driver uses a per-descriptor cache flush here;
     * wbinvd is the blunt, correct version and this loop is not hot. */
    for (int guard = 0; guard < 100000; guard++) {
        cache_sync();
        if (tx_ring[i].upper & DESC_DD)
            return len;
    }
    return -1;                                   /* the card never took it     */
}

static void tx_probe(void)
{
    uint8_t frame[60];
    for (int i = 0; i < 6; i++)
        frame[i] = 0xFF;                         /* broadcast destination     */
    for (int i = 0; i < 6; i++)
        frame[6 + i] = mac[i];                    /* our own source address    */
    frame[12] = 0x08; frame[13] = 0x00;           /* ethertype IPv4 (payload is
                                                     * filler the switch drops;
                                                     * we only test TX writeback) */
    for (int i = 14; i < 60; i++)
        frame[i] = (uint8_t)i;

    int r = e1000_tx(frame, 60);
    if (r > 0)
        klog("e1000: tx probe %d bytes, descriptor done", r);
    else
        serial_write("e1000: tx probe FAILED - card never took the frame\n");
}

int e1000_rx(uint8_t *out, int maxlen)
{
    if (!ready)
        return 0;

    struct rxd *d = &rx_ring[rdh];
    cache_sync();                                /* drop stale cache lines     */
    if (!(d->status & DESC_DD))
        return 0;                                /* nothing has arrived        */

    int len = (int)d->length;
    if (len > maxlen)
        len = maxlen;
    uint8_t *src = rx_buf[rdh];
    for (int o = 0; o < len; o++)
        out[o] = src[o];

    d->status = 0;                               /* hand the buffer back       */
    rdh = (rdh + 1) % RING;
    rdt = (rdh + RING - 1) % RING;
    w32(R_RDT, rdt);
    return len;
}
