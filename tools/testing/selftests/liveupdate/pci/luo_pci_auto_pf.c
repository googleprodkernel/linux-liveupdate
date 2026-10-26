// SPDX-License-Identifier: GPL-2.0-only
/*
 * luo_pci, but PFs are preserved automatically along with their VFs rather
 * than explicitly. See luo_pci.c.
 */
#define LUO_PCI_AUTO_PF
#include "luo_pci.c"
