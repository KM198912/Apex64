#pragma once

#include <stdint.h>
#include <bus/pci.h>

/* Attach legacy IDE controller (PCI class 0x01 subclass 0x01) */
int ide_attach(struct pci_device *dev);

/* Identify a drive on a channel (0 = primary, 1 = secondary) and head (0=master,1=slave).
 * Returns 0 on success and fills out_buf (must be 512 bytes).
 */
int ide_identify_drive(int channel, int drive, void *out_buf);
