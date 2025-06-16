// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>
//

/**
 * NVMe MMIO operations wrapper: functions and bitfield accessors
 * ==============================================================
 *
 * This header provides minimal helper functions for interacting with NVMe controller registers
 * via memory-mapped I/O (MMIO). It assumes the presence of PCIe MMIO primitives defined in
 * "mmio.h".
 *
 * The goal is to keep things simple—no heuristics for timeout values, no automatic waiting after
 * enabling or disabling the controller. Each function performs a single MMIO operation, leaving
 * policy and sequencing decisions to the caller.
 *
 * Note: In some cases (e.g., CC.EN), register bits must be updated via read–modify–write
 * to avoid overwriting other configuration fields. These helpers assume the caller expects
 * only the minimal required field to be changed, and that other fields remain unmodified.
 *
 * @file nvme.h
 */
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
 * Controller Capabilities: Maximum Queue Entries Supported (MQES)
 */
static inline uint16_t
nvme_reg_cap_get_mqes(uint64_t cap)
{
	return bitfield_get(cap, 0, 16);
}

/**
 * Controller Capabilities: Contiguous Queues Required (CQR)
 */
static inline uint8_t
nvme_reg_cap_get_cqr(uint64_t cap)
{
	return bitfield_get(cap, 16, 1);
}

/**
 * Controller Capabilities: Arbitration Mechanism Supported (AMS)
 */
static inline uint8_t
nvme_reg_cap_get_ams(uint64_t cap)
{
	return bitfield_get(cap, 17, 2);
}

/**
 * Controller Capabilities: Timeout (TO)
 */
static inline uint8_t
nvme_reg_cap_get_to(uint64_t cap)
{
	return bitfield_get(cap, 24, 8);
}

/**
 * Controller Capabilities: Doorbell Stride (DSTRD)
 */
static inline uint8_t
nvme_reg_cap_get_dstrd(uint64_t cap)
{
	return bitfield_get(cap, 32, 4);
}

/**
 * Controller Capabilities: NVM Subsystem Reset Supported (NSSRS)
 */
static inline uint8_t
nvme_reg_cap_get_nssrs(uint64_t cap)
{
	return bitfield_get(cap, 36, 1);
}

/**
 * Controller Capabilities: Command Sets Supported (CSS)
 */
static inline uint8_t
nvme_reg_cap_get_css(uint64_t cap)
{
	return bitfield_get(cap, 37, 8);
}

/**
 * Controller Capabilities: Boot Partition Support (BPS)
 */
static inline uint8_t
nvme_reg_cap_get_bps(uint64_t cap)
{
	return bitfield_get(cap, 45, 1);
}

/**
 * Controller Capabilities: Controller Power Scope (CPS)
 */
static inline uint8_t
nvme_reg_cap_get_cps(uint64_t cap)
{
	return bitfield_get(cap, 46, 2);
}

/**
 * Controller Capabilities: Memory Page Size Minimum (MPSMIN)
 */
static inline uint8_t
nvme_reg_cap_get_mpsmin(uint64_t cap)
{
	return bitfield_get(cap, 48, 4);
}

/**
 * Controller Capabilities: Memory Page Size Maximum (MPSMAX)
 */
static inline uint8_t
nvme_reg_cap_get_mpsmax(uint64_t cap)
{
	return bitfield_get(cap, 52, 4);
}

/**
 * Controller Capabilities: Persistent Memory Region Supported (PMRS)
 */
static inline uint8_t
nvme_reg_cap_get_pmrs(uint64_t cap)
{
	return bitfield_get(cap, 56, 1);
}

/**
 * Controller Capabilities: Controller Memory Buffer Supported (CMBS)
 */
static inline uint8_t
nvme_reg_cap_get_cmbs(uint64_t cap)
{
	return bitfield_get(cap, 57, 1);
}

/**
 * Controller Capabilities: NVM Subsystem Shutdown Supported (NSSS)
 */
static inline uint8_t
nvme_reg_cap_get_nsss(uint64_t cap)
{
	return bitfield_get(cap, 58, 1);
}

/**
 * Controller Capabilities: Controller Ready Modes Supported (CRMS)
 */
static inline uint8_t
nvme_reg_cap_get_crms(uint64_t cap)
{
	return bitfield_get(cap, 59, 2);
}

/**
 * Controller Capabilities: NVM Subsystem Shutdown Enhancements Supported (NSSES)
 */
static inline uint8_t
nvme_reg_cap_get_nsses(uint64_t cap)
{
	return bitfield_get(cap, 61, 1);
}

/**
 * Setup admin-queue (aq) properties
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
nvme_mmio_aq_setup(void *bar0, uint64_t asq, uint64_t acq)
{
	mmio_write32(bar0, NVME_REG_AQA, (0 << 16) | 0); // 1-entry queues
	mmio_write64(bar0, NVME_REG_ASQ, asq);
	mmio_write64(bar0, NVME_REG_ACQ, acq);
}

/**
 * Enable the current controller configuration
 */
static inline void
nvme_mmio_cc_enable(uint8_t *bar0)
{
	uint32_t cc = mmio_read32(bar0, NVME_REG_CC);

	mmio_write64(bar0, NVME_REG_CC, cc | 0x1);
}

/**
 * Disable the current controller-configuration.
 *
 * Note: Disabling takes effect asynchronously. Thus, you must wait for the controller to report
 * CSTS.RDY = 0 before proceeding. Use nvme_mmio_wait_until_not_ready() with a proper timeout
 * value.
 */
static inline void
nvme_mmio_cc_disable(uint8_t *bar0)
{
	uint32_t cc = mmio_read32(bar0, NVME_REG_CC);

	mmio_write32(bar0, NVME_REG_CC, cc & ~0x1);
}

/**
 * Wait until CSTS.RDY == 1 (controller is ready)
 *
 * Note: a sound choice for timeout_us would be CAP.TO, also read the spec on the other sensible
 * timeout choices based on CC.CRIME and CRTO.CRWMT.
 */
static inline int
nvme_mmio_csts_wait_until_ready(uint8_t *mmio, int timeout_us)
{
	for (uint64_t elapsed = 0; elapsed < timeout_us; elapsed += 1000) {
		if ((mmio_read32(mmio, NVME_REG_CSTS) & 0x1) == 0x1) {
			return 0;
		}
		usleep(1000);
	}
	return -ETIMEDOUT;
}

/**
 * Wait until CSTS.RDY == 0 (controller is not ready)
 *
 * Note: a sound choice for timeout_us would be CAP.TO, also read the spec on the other sensible
 * timeout choices based on CC.CRIME and CRTO.CRWMT.
 */
static inline int
nvme_mmio_csts_wait_until_not_ready(uint8_t *mmio, int timeout_us)
{
	for (uint64_t elapsed = 0; elapsed < timeout_us; elapsed += 1000) {
		if ((mmio_read32(mmio, NVME_REG_CSTS) & 0x1) == 0x0) {
			return 0;
		}
		usleep(1000);
	}
	return -ETIMEDOUT;
}
