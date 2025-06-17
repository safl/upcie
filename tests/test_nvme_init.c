// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

#include <upcie/upcie.h>

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

/**
 * These values are derived from the capbilities register but provided here for convenience
 */
struct nvme_controller_capabilities {
	uint8_t doorbell_stride;
};

struct nvme_controller {
	uint64_t cap; ///< Controller-capabilities register

	uint32_t vs;   ///< Controller version
	uint32_t cc;   ///< Controller configuration
	uint32_t csts; ///< Controller state

	void *asq; ///< Admin submission queue
	void *acq; ///< Admin completion queue
	void *buf; ///< Payload buffer -- now for identify command result
};

void
nvme_controller_term(struct nvme_controller *ctrlr)
{
	if (ctrlr->acq) {
		hostmem_dma_free(ctrlr->acq);
		ctrlr->acq = NULL;
	}
	if (ctrlr->asq) {
		hostmem_dma_free(ctrlr->asq);
		ctrlr->asq = NULL;
	}
	if (ctrlr->buf) {
		hostmem_dma_free(ctrlr->buf);
		ctrlr->buf = NULL;
	}
}

int
nvme_controller_init(void *bar0, struct nvme_controller *controller)
{
	int timeout;
	int err;

	printf("# Starting controller initialization...\n");

	controller->cc = nvme_mmio_cc_read(bar0);
	controller->cap = nvme_mmio_cap_read(bar0);

	controller->csts = nvme_mmio_csts_read(bar0);
	nvme_reg_csts_pr(controller->csts);

	timeout = nvme_reg_cap_get_to(controller->cap) * 500;

	printf("# Disabling...\n");
	nvme_mmio_cc_disable(bar0);

	printf("# Disabling... wait for it...\n");
	err = nvme_mmio_csts_wait_until_not_ready(bar0, timeout);
	if (err) {
		printf("FAILED: nvme_mmio_csts_wait_until_ready(); err(%d)\n", err);
		return -err;
	}
	printf("# Disabled!\n");

	controller->csts = nvme_mmio_csts_read(bar0);
	nvme_reg_csts_pr(controller->csts);

	{
		uint64_t asq, acq;

		asq = hostmem_dma_v2p(controller->asq);
		acq = hostmem_dma_v2p(controller->acq);

		nvme_mmio_aq_setup(bar0, asq, acq, 32);

		printf("asq(0x%"PRIx64"), acq(0x%"PRIx64")\n", asq, acq);
	}

	{
		uint32_t cc = 0x0;

		cc |= nvme_reg_cc_set_css(cc, 0x0);
		cc |= nvme_reg_cc_set_mps(cc, 0x0);
		cc |= nvme_reg_cc_set_ams(cc, 0x0);
		cc |= nvme_reg_cc_set_iosqes(cc, 6);
		cc |= nvme_reg_cc_set_iocqes(cc, 4);
		cc |= nvme_reg_cc_set_en(cc, 0x1);

		printf("# Enabling cc(0x%" PRIx32 ")...\n", cc);
		nvme_mmio_cc_write(bar0, cc);
	}

	printf("# Enabling... wait for it...\n");
	err = nvme_mmio_csts_wait_until_ready(bar0, timeout);
	if (err) {
		printf("FAILED: nvme_mmio_csts_wait_until_ready(); err(%d)\n", err);
		return -err;
	}
	printf("# Enabled!\n");

	return 0;
}

int
nvme_identify_controller(uint8_t *mmio, struct nvme_controller *controller)
{
	struct nvme_command cmd = {0};

	cmd.opc = NVME_ADMIN_IDENTIFY;
	cmd.cid = 0x1234;
	cmd.prp1 = hostmem_dma_v2p(controller->buf);
	cmd.cdw10 = 1; // CNS=1: Identify Controller

	memcpy(controller->asq, &cmd, 64);

	mmio_read64(mmio, NVME_REG_CAP);

	mmio_write32(mmio, NVME_REG_SQ0TDBL, 1);

	// Wait for completion
	for (int i = 0; i < 10000; ++i) {
		if (mmio_read32(mmio, NVME_REG_CQ0HDBL) != 0)
			break;
		usleep(1000);
	}

	struct nvme_completion *cpl = controller->acq;
	if ((cpl->status & 0xFFFE) != 0) {
		printf("ERR: Identify failed, status=0x%04x\n", cpl->status);
		return -EIO;
	}

	char mn[41] = {0};
	memcpy(mn, (char *)controller->buf + 24, 40);
	printf("Identify Controller: MN = %.40s\n", mn);
	return 0;
}

int
main(int argc, char **argv)
{
	struct nvme_controller controller = {0};
	struct pci_func func = {0};
	uint8_t *mmio;
	int err;

	if (argc != 2) {
		printf("Usage: %s <PCI-BDF>\n", argv[0]);
		return 1;
	}

	err = pci_func_open(argv[1], &func);
	if (err) {
		printf("Failed to open PCI device: %s\n", argv[1]);
		return -err;
	}

	err = pci_bar_map(func.bdf, 0, &func.bars[0]);
	if (err) {
		printf("Failed to map BAR0\n");
		return -err;
	}
	mmio = func.bars[0].region;

	err = hostmem_dma_init(1024 * 1024 * 128ULL);
	if (err) {
		printf("failed hostmem_dma_init(); err(%d)\n", err);
		return -err;
	}

	controller.asq = hostmem_dma_malloc(4096);
	if (!controller.asq) {
		printf("failed hostmem_dma_malloc(asq); errno(%d)\n", errno);
		return -errno;
	}
	memset(controller.asq, 0, 4096);

	controller.acq = hostmem_dma_malloc(4096);
	if (!controller.acq) {
		printf("failed hostmem_dma_malloc(acq); errno(%d)\n", errno);
		return -errno;
	}
	memset(controller.acq, 0, 4096);

	controller.buf = hostmem_dma_malloc(4096);
	if (!controller.buf) {
		printf("failed hostmem_dma_malloc(buf); errno(%d)\n", errno);
		return -errno;
	}
	memset(controller.buf, 0, 4096);

	// If this works; then MMIO is functional
	nvme_reg_cap_pr(mmio_read64(mmio, NVME_REG_CAP));
	printf("VS(0x%" PRIx32 ")\n", mmio_read32(mmio, NVME_REG_VS));

	err = nvme_controller_init(mmio, &controller);
	if (err) {
		printf("nvme_ctrlr_init(); failed\n");
		return -err;
	}

	err = nvme_identify_controller(mmio, &controller);
	if (err) {
		printf("FAILED: nvme_identify_ctrlr()\n");
		return -err;
	}

	printf("DATA\n");
	for (int i = 0; i < 4096; ++i) {
		uint8_t val = ((uint8_t *)controller.buf)[i];

		if (val) {
			printf("i(%d): 0x%d\n", i, val);
		}
	}

exit:
	pci_func_close(&func);

	nvme_controller_term(&controller);

	hostmem_dma_free(controller.asq);
	hostmem_dma_free(controller.acq);
	hostmem_dma_free(controller.buf);

	return 0;
}
