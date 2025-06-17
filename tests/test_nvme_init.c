// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

#include <upcie/upcie.h>

#define NVME_ADMIN_IDENTIFY 0x06

struct nvme_completion {
	uint32_t cdw0;
	uint32_t rsvd;
	uint16_t sqhd;
	uint16_t sqid;
	uint16_t cid;
	uint16_t status;
};

int
nvme_controller_bringup(void *bar0, struct nvme_controller *ctrlr, struct nvme_qp *aq)
{
	int timeout;
	int err;

	printf("# Starting controller initialization...\n");
	nvme_reg_csts_pr(ctrlr->csts);

	timeout = nvme_reg_cap_get_to(ctrlr->cap) * 500;

	printf("# Disabling...\n");
	nvme_mmio_cc_disable(bar0);

	printf("# Wait until not ready...\n");
	err = nvme_mmio_csts_wait_until_not_ready(bar0, timeout);
	if (err) {
		printf("FAILED: nvme_mmio_csts_wait_until_ready(); err(%d)\n", err);
		return -err;
	}
	printf("# SUCCESS\n");

	printf("# Status\n");
	ctrlr->csts = nvme_mmio_csts_read(bar0);
	nvme_reg_csts_pr(ctrlr->csts);

	{
		uint32_t cc;

		err = nvme_qp_init(aq, 0, 32, ctrlr);
		if (err) {
			return -err;
		}

		nvme_mmio_aq_setup(bar0, hostmem_dma_v2p(aq->sq), hostmem_dma_v2p(aq->cq), aq->depth);

		cc = ctrlr->cc;
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
nvme_identify_controller(struct nvme_qp *qp, void *buf)
{
	struct nvme_command cmd = {0};
	struct nvme_cmd *dwords = (void *)&cmd;

	cmd.opc = NVME_ADMIN_IDENTIFY;
	cmd.cid = 0x1234;
	cmd.prp1 = hostmem_dma_v2p(buf);
	cmd.cdw10 = 1; // CNS=1: Identify Controller

	nvme_qp_submit(qp, dwords);
	nvme_qp_sqdb_ring(qp);

	// Wait for completion
	for (int i = 0; i < 10000; ++i) {
		if (mmio_read32(qp->cqdb, 0) != 0) {
			break;
		}
		usleep(1000);
	}

	struct nvme_completion *cpl = qp->cq;
	if ((cpl->status & 0xFFFE) != 0) {
		printf("ERR: Identify failed, status=0x%04x\n", cpl->status);
		return -EIO;
	}

	return 0;
}

int
main(int argc, char **argv)
{
	struct nvme_controller ctrlr = {0};
	void *buf, *bar0;
	int err;

	if (argc != 2) {
		printf("Usage: %s <PCI-BDF>\n", argv[0]);
		return 1;
	}

	err = hostmem_dma_init(1024 * 1024 * 128ULL);
	if (err) {
		printf("failed hostmem_dma_init(); err(%d)\n", err);
		return -err;
	}

	buf = hostmem_dma_malloc(4096);
	if (!buf) {
		printf("failed hostmem_dma_malloc(buf); errno(%d)\n", errno);
		return -errno;
	}
	memset(buf, 0, 4096);

	err = nvme_controller_open(argv[1], &ctrlr);
	if (err) {
		return -errno;
	}
	bar0 = ctrlr.bar0;


	struct nvme_qp aq = { 0};

	err = nvme_controller_bringup(bar0, &ctrlr, &aq);
	if (err) {
		printf("nvme_ctrlr_init(); failed\n");
		return -err;
	}

	err = nvme_identify_controller(&aq, &ctrlr);
	if (err) {
		printf("FAILED: nvme_identify_ctrlr()\n");
		return -err;
	}

	printf("DATA\n");
	for (int i = 0; i < 4096; ++i) {
		uint8_t val = ((uint8_t *)buf)[i];

		if (val) {
			printf("i(%d): 0x%d\n", i, val);
		}
	}

	nvme_controller_close(&ctrlr);

	return 0;
}
