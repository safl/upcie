// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

#define _UPCIE_WITH_NVME
#include <upcie/upcie.h>

struct rte {
	struct hostmem_config config;
	struct hostmem_heap heap;
};

struct nvme {
	struct nvme_controller ctrlr;
	struct nvme_qpair ioq;
};

int
rte_init(struct rte *rte)
{
	int err;

	err = hostmem_config_init(&rte->config);
	if (err) {
		printf("FAILED: hostmem_config_init(); err(%d)\n", err);
		return err;
	}

	err = hostmem_heap_init(&rte->heap, 1024 * 1024 * 128ULL, &rte->config);
	if (err) {
		printf("FAILED: hostmem_heap_init(); err(%d)\n", err);
		return -err;
	}

	return 0;
}

int
nvme_init(struct nvme *nvme, const char *bdf, struct rte *rte)
{
	struct nvme_completion cpl = {0};
	struct nvme_command cmd = {0};
	int err;

	err = nvme_controller_open(&nvme->ctrlr, bdf, &rte->heap);
	if (err) {
		printf("FAILED: nvme_device_open(); err(%d)\n", err);
		return -err;
	}

	cmd.opc = 0x6; ///< IDENTIFY
	cmd.prp1 = hostmem_dma_v2p(&rte->heap, nvme->ctrlr.buf);
	cmd.cdw10 = 1; // CNS=1: Identify Controller

	err = nvme_qpair_submit_sync(&nvme->ctrlr.aq, &cmd, nvme->ctrlr.timeout_ms, &cpl);
	if (err) {
		printf("FAILED: nvme_qpair_submit_sync(); err(%d)\n", err);
		nvme_controller_close(&nvme->ctrlr);
		return err;
	}

	printf("SN('%.*s')\n", 20, ((uint8_t *)nvme->ctrlr.buf) + 4);
	printf("MN('%.*s')\n", 40, ((uint8_t *)nvme->ctrlr.buf) + 24);

	err = nvme_controller_create_io_qpair(&nvme->ctrlr, &nvme->ioq, 32);
	if (err) {
		printf("FAILED: nvme_device_create_io_qpair(); err(%d)\n", err);
	}

	return 0;
}

int
main(int argc, char **argv)
{
	struct nvme nvme = {0};
	struct rte rte = {0};
	int err;

	if (argc != 2) {
		printf("Usage: %s <PCI-BDF>\n", argv[0]);
		return 1;
	}

	err = rte_init(&rte);
	if (err) {
		printf("FAILED: rte_init();");
		return -err;
	}

	err = nvme_init(&nvme, argv[1], &rte);
	if (err) {
		printf("FAILED: nvme_init();");
		return -err;
	}

	nvme_controller_close(&nvme.ctrlr);
	hostmem_heap_term(&rte.heap);

	return err;
}
