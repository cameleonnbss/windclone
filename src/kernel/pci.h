/* WindClone — PCI configuration-space enumeration.
 * pci_init() walks every bus/device/function once at boot and logs
 * what it found; the counts are readable afterwards.
 *
 * Beyond the census, a driver needs three things from this file:
 *   - to find a device by vendor+device id (pci_find),
 *   - to read and write that device's configuration bytes (the cfg_*
 *     accessors, over mechanism #1),
 *   - and to turn memory accesses on (pci_enable_mmio_and_bus_master),
 *     because a device that boots with its BARs still zeroed answers
 *     every MMIO read with 0xFFFFFFFF forever.
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_PCI_H
#define WINDCLONE_PCI_H

#include <stdint.h>

void pci_init(void);
int  pci_devices(void);
int  pci_mmio_bars(void);

/* Fill out[0..2] with bus/device/function of the first device matching this
 * vendor+device id. Returns 1 on a match, 0 if the bus has no such device. */
int  pci_find(uint16_t vendor, uint16_t device, uint8_t out[3]);

/* Configuration-space accessors for one (bus,dev,func) slot. Widths 8/16/32. */
uint8_t  pci_cfg_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
uint16_t pci_cfg_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
void pci_cfg_write8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint8_t v);
void pci_cfg_write16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint16_t v);
void pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t v);

/* Set the memory-space and bus-master bits of the command register so the
 * device decodes its MMIO BAR and may drive DMA. Idempotent. */
void pci_enable_mmio_and_bus_master(uint8_t bus, uint8_t dev, uint8_t func);

/* The raw 64-bit BAR (or 32-bit for a non-prefetch bar), already masked of its
 * type bits, for BAR `n` of a slot. Returns 0 when the BAR is unset. */
uint64_t pci_bar(uint8_t bus, uint8_t dev, uint8_t func, int n);

#endif
