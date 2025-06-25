// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

#include <upcie/upcie.h>
#include <nvme.h>
#include <nvme_command.h>
#include <nvme_controller.h>
#include <nvme_request.h>
#include <nvme_qpair.h>

/**
 * This is one way of combining the various components needed
 */
struct nvme_device {
	struct pci_func func; ///< The PCIe function and mapped bars
	struct nvme_controller ctrlr;
	struct nvme_request_pool aqrp;
	struct nvme_qpair aq;
	void *buf;
};

void
nvme_device_close(struct nvme_device *dev)
{
	hostmem_dma_free(dev->buf);
	nvme_controller_term(&dev->ctrlr);
	pci_func_close(&dev->func);
}

/**
 * Disables the NVMe controller at 'bdf', sets up admin-queues and enables it again
 */
int
nvme_device_open(struct nvme_device *dev, const char *bdf)
{
	uint32_t cc = 0;
	int err;

	memset(dev, 0, sizeof(*dev));

	dev->buf = hostmem_dma_malloc(4096);
	if (!dev->buf) {
		printf("FAILED: hostmem_dma_malloc(buf); errno(%d)\n", errno);
		return -errno;
	}
	memset(dev->buf, 0, 4096);

	err = pci_func_open(bdf, &dev->func);
	if (err) {
		printf("Failed to open PCI device: %s\n", bdf);
		return -err;
	}

	err = pci_bar_map(dev->func.bdf, 0, &dev->func.bars[0]);
	if (err) {
		printf("Failed to map BAR0\n");
		return -err;
	}

	err = nvme_controller_init(&dev->ctrlr, dev->func.bars[0].region);
	if (err) {
		return -errno;
	}

	nvme_request_pool_init(&dev->aqrp);

	nvme_mmio_cc_disable(dev->ctrlr.bar0);

	err = nvme_mmio_csts_wait_until_not_ready(dev->ctrlr.bar0, dev->ctrlr.timeout_ms);
	if (err) {
		printf("FAILED: nvme_mmio_csts_wait_until_ready(); err(%d)\n", err);
		return -err;
	}

	dev->ctrlr.csts = nvme_mmio_csts_read(dev->ctrlr.bar0);

	err = nvme_qpair_init(&dev->aq, 0, 256, &dev->ctrlr);
	if (err) {
		printf("FAILED: nvme_qpair_init(); err(%d)\n", err);
		return -err;
	}

	nvme_mmio_aq_setup(dev->ctrlr.bar0, hostmem_dma_v2p(dev->aq.sq),
			   hostmem_dma_v2p(dev->aq.cq), dev->aq.depth);

	cc = nvme_reg_cc_set_css(cc, 0x0);
	cc = nvme_reg_cc_set_shn(cc, 0x0);
	cc = nvme_reg_cc_set_mps(cc, 0x0);
	cc = nvme_reg_cc_set_ams(cc, 0x0);
	cc = nvme_reg_cc_set_iosqes(cc, 6);
	cc = nvme_reg_cc_set_iocqes(cc, 6);
	cc = nvme_reg_cc_set_en(cc, 0x1);

	nvme_mmio_cc_write(dev->ctrlr.bar0, cc);

	err = nvme_mmio_csts_wait_until_ready(dev->ctrlr.bar0, dev->ctrlr.timeout_ms);
	if (err) {
		printf("FAILED: nvme_mmio_csts_wait_until_ready(); err(%d)\n", err);
		return -err;
	}

	return 0;
}

int
main(int argc, char **argv)
{
	struct nvme_device dev = {0};
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

	err = nvme_device_open(&dev, argv[1]);
	if (err) {
		printf("FAILED: nvme_device_open(); err(%d)\n", err);
		return -err;
	}

	{
		struct nvme_command cmd = {0};
		struct nvme_completion *cpl;

		cmd.opc = 0x6; ///< IDENTIFY
		cmd.cid = 0x1;
		cmd.prp1 = hostmem_dma_v2p(dev.buf);
		cmd.cdw10 = 1; // CNS=1: Identify Controller

		printf("cmd.prp1(0x%" PRIx64 ")\n", cmd.prp1);

		nvme_qpair_submit(&dev.aq, &dev.aqrp, &cmd, NULL);
		nvme_qpair_sqdb_ring(&dev.aq);

		cpl = nvme_qpair_reap_cpl(&dev.aq, dev.ctrlr.timeout_ms);
		if (!cpl) {
			printf("NO COMPLETION!\n");
			return -EIO;
		}
	}

	printf("SN('%.*s')\n", 20, ((uint8_t *)dev.buf) + 4);
	printf("MN('%.*s')\n", 40, ((uint8_t *)dev.buf) + 24);

	nvme_device_close(&dev);

	return 0;
}
