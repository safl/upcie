// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

#include <upcie/upcie.h>
#include <nvme.h>
#include <nvme_command.h>
#include <nvme_controller.h>
#include <nvme_request.h>
#include <nvme_queue.h>

int
main(int argc, char **argv)
{
	struct nvme_controller ctrlr = {0};
	struct nvme_request_pool pool = {0};
	struct nvme_qp aq = {0};
	void *buf, *bar0;
	int timeout_ms, err;

	if (argc != 2) {
		printf("Usage: %s <PCI-BDF>\n", argv[0]);
		return 1;
	}

	nvme_request_pool_init(&pool);

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
	timeout_ms = nvme_reg_cap_get_to(ctrlr.cap) * 500;

	printf("# Starting controller initialization...\n");
	nvme_reg_csts_pr(ctrlr.csts);

	printf("# Disabling...\n");
	nvme_mmio_cc_disable(bar0);

	printf("# Wait until not ready...\n");
	err = nvme_mmio_csts_wait_until_not_ready(bar0, timeout_ms);
	if (err) {
		printf("FAILED: nvme_mmio_csts_wait_until_ready(); err(%d)\n", err);
		return -err;
	}
	printf("# SUCCESS\n");

	printf("# Status\n");
	ctrlr.csts = nvme_mmio_csts_read(bar0);
	nvme_reg_csts_pr(ctrlr.csts);

	{
		uint64_t buf_phys;
		uint32_t cc;

		err = nvme_qp_init(&aq, 0, 256, &ctrlr);
		if (err) {
			return -err;
		}

		buf_phys = hostmem_dma_v2p(buf);

		nvme_mmio_aq_setup(bar0, hostmem_dma_v2p(aq.sq), hostmem_dma_v2p(aq.cq), aq.depth);

		// cc = ctrlr.cc;
		cc = 0;
		cc = nvme_reg_cc_set_css(cc, 0x0);
		cc = nvme_reg_cc_set_shn(cc, 0x0);
		cc = nvme_reg_cc_set_mps(cc, 0x0);
		cc = nvme_reg_cc_set_ams(cc, 0x0);
		cc = nvme_reg_cc_set_iosqes(cc, 6);
		cc = nvme_reg_cc_set_iocqes(cc, 6);
		cc = nvme_reg_cc_set_en(cc, 0x1);

		printf("# Enabling cc(0x%" PRIx32 ")...\n", cc);
		nvme_mmio_cc_write(bar0, cc);
	}

	printf("# Enabling... wait for it...\n");
	err = nvme_mmio_csts_wait_until_ready(bar0, timeout_ms);
	if (err) {
		printf("FAILED: nvme_mmio_csts_wait_until_ready(); err(%d)\n", err);
		return -err;
	}
	printf("# Enabled!\n");

	{
		struct nvme_command cmd = {0};
		struct nvme_completion *cpl;

		cmd.opc = 0x6; ///< IDENTIFY
		cmd.cid = 0x1;
		cmd.prp1 = hostmem_dma_v2p(buf);
		cmd.cdw10 = 1; // CNS=1: Identify Controller

		printf("cmd.prp1(0x%" PRIx64 ")\n", cmd.prp1);

		nvme_qp_submit(&aq, &pool, &cmd, NULL);
		nvme_qp_sqdb_ring(&aq);

		cpl = nvme_qp_reap_cpl(&aq, timeout_ms);
		if (!cpl) {
			printf("NO COMPLETION!\n");
			return -EIO;
		}
	}

	printf("DATA\n");
	for (int i = 0; i < 4096; ++i) {
		uint8_t val = ((uint8_t *)buf)[i];

		if (val) {
			printf("i(%d): 0x%d\n", i, val);
		}
	}

	printf("SN('%.*s')\n", 20, ((uint8_t *)buf) + 4);
	printf("MN('%.*s')\n", 40, ((uint8_t *)buf) + 24);

	nvme_controller_close(&ctrlr);

	return 0;
}
