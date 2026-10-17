/* WindClone — PCI enumeration via configuration mechanism #1.
 *
 * Why port I/O and not MMIO (ECAM) yet: mechanism #1 (ports 0xCF8/0xCFC)
 * is guaranteed to exist on every x86/PCI machine since 1993, including
 * QEMU's i440FX and q35 chipsets. PCIe machines also expose the fancy
 * memory-mapped ECAM window, but reading it needs the BAR-0 of each
 * Root Complex register block — Phase 4+ material. One thing at a time.
 *
 * A config address is a bit-packed port write:
 *   bit 31      = enable
 *   bits 24-31  = bus
 *   bits 16-23  = device
 *   bits 11-15  = function
 *   bits 0-7    = dword-aligned register offset
 * Vendor == 0xFFFF means "nothing here". That single read is the whole
 * enumeration protocol; everything else is decoding the answer.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stddef.h>

#include "pci.h"
#include "io.h"
#include "log.h"

#define PCI_CMD   0xCF8
#define PCI_DATA  0xCFC

static int device_count;
static int mmio_bars;
static int io_bars;
static int host_bridges;

static uint32_t cfg_addr(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    return 0x80000000u | ((uint32_t)bus << 16) |
           ((uint32_t)dev << 11) | ((uint32_t)func << 8) | (off & 0xFC);
}

static uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    outl(PCI_CMD, cfg_addr(bus, dev, func, off));
    io_wait();
    return inl(PCI_DATA);
}

/* Decode the six type-0 BARs: bit 0 selects the address space,
 * memory bits 1-2 the width (16/32/64 — a 64-bit BAR eats two slots). */
static void decode_bars(uint8_t bus, uint8_t dev, uint8_t func)
{
    for (int i = 0; i < 6; i++) {
        uint32_t bar = pci_read32(bus, dev, func, (uint8_t)(0x10 + i * 4));
        if (bar == 0)
            continue;
        if (bar & 1u) {
            io_bars++;
            klog("  bar%d=io@0x%04x", i, bar & ~3u);
        } else {
            uint8_t type = (uint8_t)((bar >> 1) & 3u);
            uint64_t addr = bar & ~15u;
            const char *name = (type == 1) ? "mem16"
                           : (type == 2) ? "mem64" : "mem32";
            if (type == 2 && i < 5) {
                uint32_t hi = pci_read32(bus, dev, func,
                                         (uint8_t)(0x14 + i * 4));
                addr |= (uint64_t)hi << 32;
                i++; /* the high half consumed the next slot */
            }
            mmio_bars++;
            klog("  bar%d=%s@0x%08x", i, name, (uint32_t)addr);
        }
    }
}

void pci_init(void)
{
    klog("pci: scanning configuration space (mechanism #1, ports 0xcf8/0xcfc)");

    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint32_t dev = 0; dev < 32; dev++) {
            for (uint32_t func = 0; func < 8; func++) {
                uint32_t vd = pci_read32((uint8_t)bus, (uint8_t)dev,
                                         (uint8_t)func, 0x00);
                if (vd == 0xFFFFFFFFu || vd == 0) {
                    /* func 0 absent => no point probing 1-7 */
                    if (func == 0)
                        break;
                    continue;
                }

                uint32_t ccrev = pci_read32((uint8_t)bus, (uint8_t)dev,
                                            (uint8_t)func, 0x08);
                uint32_t hdr   = pci_read32((uint8_t)bus, (uint8_t)dev,
                                            (uint8_t)func, 0x0C);
                uint8_t  multihdr = (uint8_t)(hdr >> 16);
                uint32_t class24  = (ccrev >> 8) & 0xFFFFFFu;

                device_count++;
                klog("pci %02x:%02x.%x vendor=0x%04x device=0x%04x class=0x%06x",
                     bus, dev, func, vd & 0xFFFFu, vd >> 16, class24);

                if ((class24 >> 8) == 0x0600u)
                    host_bridges++;

                if ((multihdr & 0x7Fu) == 0)
                    decode_bars((uint8_t)bus, (uint8_t)dev, (uint8_t)func);

                /* Not a multi-function device: functions 1-7 are
                 * guaranteed absent, stop probing this slot early. */
                if ((multihdr & 0x80u) == 0)
                    break;
            }
        }
    }

    if (host_bridges > 0)
        klog("pci: host bridge present (class 0x06) - enumeration works");
    klog("pci: enumeration done: %d devices, %d mmio bars, %d io bars",
         device_count, mmio_bars, io_bars);
}

int pci_devices(void)
{
    return device_count;
}

int pci_mmio_bars(void)
{
    return mmio_bars;
}
