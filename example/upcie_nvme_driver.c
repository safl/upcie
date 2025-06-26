// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

#define _UPCIE_WITH_NVME
#include <upcie/upcie.h>

int
main(int argc, char **argv)
{
	struct nvme_controller ctrlr = {0};
	struct nvme_qpair ioq = {0};
	int err;

	if (argc != 2) {
		printf("Usage: %s <PCI-BDF>\n", argv[0]);
		return 1;
	}

	err = hostmem_dma_init(1024 * 1024 * 128ULL);
	if (err) {
		printf("FAILED: hostmem_dma_init(); err(%d)\n", err);
		return -err;
	}

	err = nvme_controller_init(&ctrlr, argv[1]);
	if (err) {
		printf("FAILED: nvme_device_open(); err(%d)\n", err);
		return -err;
	}

	{
		struct nvme_command cmd = {0};
		struct nvme_completion cpl = {0};

		cmd.opc = 0x6; ///< IDENTIFY
		cmd.prp1 = hostmem_dma_v2p(ctrlr.buf);
		cmd.cdw10 = 1; // CNS=1: Identify Controller

		err = nvme_qpair_submit_sync(&ctrlr.aq, &ctrlr.aqrp, &cmd, ctrlr.timeout_ms, &cpl);
		if (err) {
			printf("FAILED: nvme_qpair_submit_sync(); err(%d)\n", err);
			goto exit;
		}
	}

	printf("SN('%.*s')\n", 20, ((uint8_t *)ctrlr.buf) + 4);
	printf("MN('%.*s')\n", 40, ((uint8_t *)ctrlr.buf) + 24);

	err = nvme_controller_create_io_qpair(&ctrlr, &ioq, 32);
	if (err) {
		printf("FAILED: nvme_device_create_io_qpair(); err(%d)\n", err);
	}

exit:
	nvme_controller_term(&ctrlr);

	return err;
}
