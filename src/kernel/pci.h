/* WindClone — PCI configuration-space enumeration.
 * pci_init() walks every bus/device/function once at boot and logs
 * what it found; the counts are readable afterwards.
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_PCI_H
#define WINDCLONE_PCI_H

void pci_init(void);
int  pci_devices(void);
int  pci_mmio_bars(void);

#endif
