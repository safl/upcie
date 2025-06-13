// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>
/*
	A collection of helpers for NVMe-controller operations via memory-mapped io

	All the functions rely on the availabilty of mmio-functions, specifically those defined by 	pci.h,
	however, if you need a different implementation for the mmio operations that is given in pci.h,
	then you just include the same function-signatures before
*/
#include <pci.h>

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
 * @param region A memory-mapped region pointing to the start of bar0
 * @param asq DMA-able address pointing to the start of the admin submission queue
 * @param acq DMA-able address pointing to the start of the admin completion queue
 *
 */
static inline void
nvme_controller_adminq_setup(void *region, uint64_t asq, uint64_t acq)
{
	pci_region_write32(region, NVME_REG_AQA, (0 << 16) | 0); // 1-entry queues
	pci_region_write64(region, NVME_REG_ASQ, asq);
	pci_region_write64(region, NVME_REG_ACQ, acq);
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
 * @param region A memory-mapped region pointing to the start of bar0
 */
static inline void
nvme_controller_enable(uint8_t *region)
{
	uint32_t cc = (6 << 20) | (4 << 16) | 0x1;

	pci_region_write32(region, NVME_REG_CC, cc);
}

/**
 * Check the controller state
 *
 * This is done via the memory-mapped NVME_REG_CSTS register pointed to by the given 'region'
 *
 * @param region A memory-mapped region pointing to the start of bar0
 */
static inline int
nvme_controller_is_enabled(void *region)
{
	return pci_region_read32(region, NVME_REG_CSTS) & 1;
}