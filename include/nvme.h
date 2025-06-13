// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>
//

/**
 * Header-only NVMe MMIO access helpers
 *
 * This header provides simple helper functions for interacting with NVMe controller
 * registers using memory-mapped I/O (MMIO). It assumes the availability of PCIe MMIO
 * primitives defined in "pci.h".
 *
 * Custom MMIO support
 * ===================
 * If you want to use your own MMIO access functions instead of those in "pci.h", define
 * the preprocessor symbol `UPCIE_NVME_PCI_INTERFACE` before including this header. Doing
 * so will prevent inclusion of "pci.h", allowing your own definitions to take effect.
 *
 * Example:
 *     #define UPCIE_NVME_PCI_INTERFACE
 *     #include "my_mmio.h"
 *     #include "nvme.h"
 */
#ifndef UPCIE_NVME_H
#define UPCIE_NVME_H

#ifndef UPCIE_NVME_PCI_INTERFACE
#define UPCIE_NVME_PCI_INTERFACE
#include <pci.h>
#endif

#define NVME_REG_CAP 0x00
#define NVME_REG_VS 0x08
#define NVME_REG_INTMS 0x0C
#define NVME_REG_INTMC 0x10

#define NVME_REG_CC 0x14
#define NVME_REG_CSTS 0x1C
#define NVME_REG_AQA 0x24
#define NVME_REG_ASQ 0x28
#define NVME_REG_ACQ 0x30
#define NVME_REG_SQ0TDBL 0x1000
#define NVME_REG_CQ0HDBL 0x1004

/**
 * Setup admin-queue properties
 *
 * Assumptions
 * ===========
 *
 * - controller is disabled
 *
 * @param bar0 A memory-mapped region pointing to the start of bar0
 * @param asq DMA-able address pointing to the start of the admin submission queue
 * @param acq DMA-able address pointing to the start of the admin completion queue
 *
 */
static inline void
nvme_controller_adminq_setup(void *bar0, uint64_t asq, uint64_t acq)
{
	mmio_write32(bar0, NVME_REG_AQA, (0 << 16) | 0); // 1-entry queues
	mmio_write64(bar0, NVME_REG_ASQ, asq);
	mmio_write64(bar0, NVME_REG_ACQ, acq);
}

/**
 * Enable NVMe controller
 *
 * This is done via the memory-mapped NVME_REG_CC register pointed to by the given 'region'
 *
 * Assumptions
 * ===========
 *
 * - controller is disabled
 * - admin-queue properties have been setup
 *
 * @param bar0 A memory-mapped region pointing to the start of bar0
 */
static inline void
nvme_controller_enable(uint8_t *bar0)
{
	uint32_t cc = (6 << 20) | (4 << 16) | 0x1;

	mmio_write32(bar0, NVME_REG_CC, cc);
}

/**
 * Check the controller state
 *
 * This is done via the memory-mapped NVME_REG_CSTS register pointed to by the given 'region'
 *
 * @param bar0 A memory-mapped region pointing to the start of bar0
 */

static inline int
nvme_controller_is_enabled(void *bar0)
{
	return mmio_read32(bar0, NVME_REG_CSTS) & 1;
}

#endif ///< UPCIE_NVME_H