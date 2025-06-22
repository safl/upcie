struct nvme_controller {
	void *bar0;	      ///< Pointer to the BAR-region of the controller
	uint32_t csts;	      ///< Controller Status
	uint32_t cap;	      ///< Controller Capabilities
	uint32_t cc;	      ///< Controller configuration
	struct pci_func func; ///< The PCIe function and mapped bars
};

void
nvme_controller_close(struct nvme_controller *ctrlr)
{
	pci_func_close(&ctrlr->func);
	ctrlr->bar0 = NULL;
}

/**
 * This function is all about host-concerns of pci-scan/bar-map, retrieve properties, this
 * does **not** have anything to do with with CC.EN or similar. Rather it sets up the MMIO
 * based communication channel and retrieves a couple of controller register values: Status,
 * Configuration, and Capabilities. All entities required to do CC.EN=1, but host-level concerns,
 * not NVMe logic per-se.
 */
int
nvme_controller_open(const char *bdf, struct nvme_controller *ctrlr)
{
	int err;

	err = pci_func_open(bdf, &ctrlr->func);
	if (err) {
		printf("Failed to open PCI device: %s\n", bdf);
		return -err;
	}

	err = pci_bar_map(ctrlr->func.bdf, 0, &ctrlr->func.bars[0]);
	if (err) {
		printf("Failed to map BAR0\n");
		return -err;
	}

	ctrlr->bar0 = ctrlr->func.bars[0].region;
	ctrlr->cc = nvme_mmio_cc_read(ctrlr->bar0);
	ctrlr->cap = nvme_mmio_cap_read(ctrlr->bar0);
	ctrlr->csts = nvme_mmio_csts_read(ctrlr->bar0);

	return 0;
}
