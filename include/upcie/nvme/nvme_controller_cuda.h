// SPDX-License-Identifier: BSD-3-Clause

/**
 * CUDA NVMe Controller Extension
 * ==============================
 *
 * This header extends the functionality defined in the uPCIe NVMe Controller
 * header `upcie/nvme/nvme_controller.h` with functions for CUDA compatible
 * NVMe controllers.
 */


/**
 * Opens an NVMe controller with CC.MPS=4 (64 KB), matching the CUDA heap page size.
 *
 * Identical to nvme_controller_open() except CC.MPS is set to 4 (2^(12+4) = 64 KB)
 * so that PRP construction for GPU memory using cudamem_heap is correct.
 */
static inline int
nvme_controller_cuda_open(struct nvme_controller *ctrlr, const char *bdf,
			   struct hostmem_heap *heap)
{
	uint64_t cap;
	void *bar0;
	int err;

	memset(ctrlr, 0, sizeof(*ctrlr));
	ctrlr->heap = heap;

	ctrlr->buf = hostmem_dma_malloc(ctrlr->heap, 4096);
	if (!ctrlr->buf) {
		UPCIE_DEBUG("FAILED: hostmem_dma_malloc(buf); errno(%d)\n", errno);
		return -errno;
	}
	memset(ctrlr->buf, 0, 4096);

	nvme_qid_bitmap_init(ctrlr->qids);

	err = pci_func_open(bdf, &ctrlr->func);
	if (err) {
		UPCIE_DEBUG("FAILED: pci_func_open(%.*s); err(%d)", 13, bdf, err);
		return -err;
	}

	err = pci_bar_map(ctrlr->func.bdf, 0, &ctrlr->func.bars[0]);
	if (err) {
		UPCIE_DEBUG("FAILED: pci_bar_map(BAR0); err(%d)", err);
		return -err;
	}
	bar0 = ctrlr->func.bars[0].region;

	cap = nvme_mmio_cap_read(bar0);
	ctrlr->timeout_ms = nvme_reg_cap_get_to(cap) * 500;

	nvme_mmio_cc_disable(bar0);

	err = nvme_mmio_csts_wait_until_not_ready(bar0, ctrlr->timeout_ms);
	if (err) {
		UPCIE_DEBUG("FAILED: nvme_mmio_csts_wait_until_not_ready(); err(%d)\n", err);
		return -err;
	}

	err = nvme_qpair_init(&ctrlr->aq, 0, 256, ctrlr->func.bars[0].region, ctrlr->heap);
	if (err) {
		UPCIE_DEBUG("FAILED: nvme_qpair_init(); err(%d)", err);
		return -err;
	}

	nvme_mmio_aq_setup(bar0, hostmem_dma_v2p(heap, ctrlr->aq.sq),
			   hostmem_dma_v2p(heap, ctrlr->aq.cq), ctrlr->aq.depth);

	{
		uint32_t css = (nvme_reg_cap_get_css(cap) & (1 << 6)) ? 0x6 : 0x0;
		uint32_t cc = 0;

		cc = nvme_reg_cc_set_css(cc, css);
		cc = nvme_reg_cc_set_shn(cc, 0x0);
		cc = nvme_reg_cc_set_mps(cc, 4); ///< 2^(12+4) = 64 KB, matches cudamem_heap pagesize
		cc = nvme_reg_cc_set_ams(cc, 0x0);
		cc = nvme_reg_cc_set_iosqes(cc, 6);
		cc = nvme_reg_cc_set_iocqes(cc, 4);
		cc = nvme_reg_cc_set_en(cc, 0x1);

		nvme_mmio_cc_write(bar0, cc);
	}

	err = nvme_mmio_csts_wait_until_ready(bar0, ctrlr->timeout_ms);
	if (err) {
		UPCIE_DEBUG("FAILED: nvme_mmio_csts_wait_until_ready(); err(%d)", err);
		return -err;
	}

	return 0;
}

