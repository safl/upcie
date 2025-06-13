// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

#define _GNU_SOURCE
#include <assert.h>
#include <hostmem.h>
#include <mmio.h>
#include <pci.h>
#include <nvme.h>
#include <string.h>
#include <unistd.h>

#define NVME_ADMIN_IDENTIFY 0x06

struct nvme_command {
	uint8_t opc; ///< opcode
	uint8_t fuse;
	uint16_t cid; ///< command id
	uint32_t nsid;
	uint64_t rsvd2;
	uint64_t mptr;
	uint64_t prp1;
	uint64_t prp2;
	uint32_t cdw10;
	uint32_t cdw11;
	uint32_t cdw12;
	uint32_t cdw13;
	uint32_t cdw14;
	uint32_t cdw15;
};

struct nvme_completion {
	uint32_t cdw0;
	uint32_t rsvd;
	uint16_t sqhd;
	uint16_t sqid;
	uint16_t cid;
	uint16_t status;
};

struct nvme_controller {
	void *asq; ///< Admin submission queue
	void *acq; ///< Admin completion queue
	void *buf; ///< Payload buffer -- now for identify command result
};

/**
 * Extracts a bitfield from a 64-bit integer.
 *
 * @param val    The input 64-bit value to extract bits from.
 * @param offset The bit offset (starting from LSB at position 0).
 * @param width  The width of the bitfield in bits (1–64).
 *
 * @return The extracted bitfield, right-aligned to bit position 0.
 *
 * Example:
 *   bits(0xFF00, 8, 8) → 0xFF
 *
 * Note: Behavior is undefined if width is 0 or offset + width > 64.
 */
static inline uint64_t
bits(uint64_t val, uint8_t offset, uint8_t width)
{
	return (val >> offset) & ((1ULL << width) - 1);
}

static inline int
nvme_reg_cap_pr(uint64_t val)
{
	int wrtn = 0;

	wrtn += printf("nvme_reg_cap:\n");
	wrtn += printf("  mqes   : %u\n", (unsigned)bits(val, 0, 16));
	wrtn += printf("  to     : %u   # %.1f seconds\n", (unsigned)bits(val, 32, 8),
		       bits(val, 32, 8) * 0.5);
	wrtn += printf("  dstrd  : %u    # stride=%u bytes\n", (unsigned)bits(val, 37, 4),
		       4 * (1U << bits(val, 37, 4)));
	wrtn += printf("  css    : %#x\n", (unsigned)bits(val, 43, 4));
	wrtn += printf("  mpsmin : %u    # page=%u KiB\n", (unsigned)bits(val, 48, 4),
		       1U << (12 + bits(val, 48, 4)));
	wrtn += printf("  mpsmax : %u    # page=%u KiB\n", (unsigned)bits(val, 52, 4),
		       1U << (12 + bits(val, 52, 4)));

	return wrtn;
}

uint64_t
vtp(void *virt)
{
	uint64_t phys;
	int err;

	err = hostmem_buffer_virt_to_phys(&g_hostmem_state.heap, virt, &phys);
	assert(!err);

	printf("phys(%" PRIu64 ")\n", phys);

	return phys;
}

void
nvme_ctrlr_term(struct nvme_controller *ctrlr)
{
	if (ctrlr->acq) {
		hostmem_buffer_free(&g_hostmem_state.heap, ctrlr->acq);
		ctrlr->acq = NULL;
	}
	if (ctrlr->asq) {
		hostmem_buffer_free(&g_hostmem_state.heap, ctrlr->asq);
		ctrlr->asq = NULL;
	}
	if (ctrlr->buf) {
		hostmem_buffer_free(&g_hostmem_state.heap, ctrlr->buf);
		ctrlr->buf = NULL;
	}
}

int
nvme_ctrlr_init(uint8_t *mmio, struct nvme_controller *ctrlr)
{
	uint32_t cc;
	int err;

	// Disable controller if needed
	if (mmio_read32(mmio, NVME_REG_CSTS) & 1) {
		mmio_write32(mmio, NVME_REG_CC, 0);

		int timeout_us = 1000 * 1000; // 1 second
		for (int i = 0; i < timeout_us / 1000; ++i) {
			if ((mmio_read32(mmio, NVME_REG_CSTS) & 1) == 0)
				break;
			usleep(1000);
		}
		if (mmio_read32(mmio, NVME_REG_CSTS) & 1) {
			fprintf(stderr, "WARN: Controller did not disable (CSTS.RDY still set)\n");
		}
	}

	nvme_controller_adminq_setup(mmio, vtp(ctrlr->asq), vtp(ctrlr->acq));

	nvme_controller_enable(mmio);

	// Wait for CSTS.RDY == 1
	{
		int timeout_us = 1000 * 1000; // 1 second
		for (int i = 0; i < timeout_us / 1000; ++i) {
			if (mmio_read32(mmio, NVME_REG_CSTS) & 1) {
				return 0; // ready
			}
			usleep(1000);
		}
		fprintf(stderr, "ERR: Controller failed to become ready\n");
		return -ETIMEDOUT;
	}
}

int
nvme_identify_ctrlr(uint8_t *mmio, struct nvme_controller *ctrlr)
{
	struct nvme_command cmd = {0};

	cmd.opc = NVME_ADMIN_IDENTIFY;
	cmd.cid = 0x1234;
	cmd.prp1 = vtp(ctrlr->buf);
	cmd.cdw10 = 1; // CNS=1: Identify Controller

	memcpy(ctrlr->asq, &cmd, 64);

	mmio_write32(mmio, NVME_REG_SQ0TDBL, 1);

	// Wait for completion
	for (int i = 0; i < 10000; ++i) {
		if (mmio_read32(mmio, NVME_REG_CQ0HDBL) != 0)
			break;
		usleep(1000);
	}

	struct nvme_completion *cpl = ctrlr->acq;
	if ((cpl->status & 0xFFFE) != 0) {
		fprintf(stderr, "ERR: Identify failed, status=0x%04x\n", cpl->status);
		return -EIO;
	}

	char mn[41] = {0};
	memcpy(mn, (char *)ctrlr->buf + 24, 40);
	printf("Identify Controller: MN = %.40s\n", mn);
	return 0;
}

int
main(int argc, char **argv)
{
	struct nvme_controller ctrlr = {0};
	struct pci_func func = {0};
	uint8_t *mmio;
	int err;

	if (argc != 2) {
		fprintf(stderr, "Usage: %s <PCI-BDF>\n", argv[0]);
		return 1;
	}

	err = pci_func_open(argv[1], &func);
	if (err) {
		fprintf(stderr, "Failed to open PCI device: %s\n", argv[1]);
		return -err;
	}

	err = pci_bar_map(func.bdf, 0, &func.bars[0]);
	if (err) {
		fprintf(stderr, "Failed to map BAR0\n");
		return 1;
	}

	err = hostmem_state_init(&g_hostmem_state);
	if (err) {
		fprintf(stderr, "Failed to init hugepages\n");
		return 1;
	}

	err = hostmem_heap_init(&g_hostmem_state.heap, 1024 * 1024 * 1024 * 1ULL);
	if (err) {
		fprintf(stderr, "Failed to init heap\n");
		return 1;
	}

	ctrlr.asq = hostmem_buffer_alloc(&g_hostmem_state.heap, 4096);
	ctrlr.acq = hostmem_buffer_alloc(&g_hostmem_state.heap, 4096);
	ctrlr.buf = hostmem_buffer_alloc(&g_hostmem_state.heap, 4096);

	assert(ctrlr.asq);
	assert(ctrlr.acq);
	assert(ctrlr.buf);

	memset(ctrlr.asq, 0, 4096);
	memset(ctrlr.acq, 0, 4096);
	memset(ctrlr.buf, 0, 4096);

	mmio = func.bars[0].region;

	// Verify mmio is functional
	{
		uint64_t v64;
		uint32_t v32;

		nvme_reg_cap_pr(mmio_read64(mmio, NVME_REG_CAP));

		v32 = mmio_read32(mmio, NVME_REG_VS);
		printf("VS(0x%" PRIx32 ")\n", v32);
	}

	err = nvme_ctrlr_init(mmio, &ctrlr);
	if (err) {
		printf("nvme_ctrlr_init(); failed\n");
		return -err;
	}

	err = nvme_identify_ctrlr(mmio, &ctrlr);
	if (err) {
		printf("FAILED: nvme_identify_ctrlr()\n");
		return -err;
	}

	for (int i = 0; i < 4096; ++i) {
		printf("i(%d): 0x%d\n", i, ((uint8_t *)ctrlr.buf)[i]);
	}

	// Cleanup
	pci_func_close(&func);

	nvme_ctrlr_term(&ctrlr);

	hostmem_buffer_free(&g_hostmem_state.heap, ctrlr.acq);
	hostmem_buffer_free(&g_hostmem_state.heap, ctrlr.asq);
	hostmem_buffer_free(&g_hostmem_state.heap, ctrlr.buf);

	return 0;
}
