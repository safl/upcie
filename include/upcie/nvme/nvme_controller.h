// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * Rudimentary Representation of Controller, BAR Mapping, Registers, and Derived Values
 * ====================================================================================
 *
 * This header defines basic structures and access patterns for working with an NVMe controller,
 * including BAR-space mappings, controller registers, and values derived from register content.
 *
 * @file nvme_controller.h
 * @version 0.1.0
 */

struct nvme_controller {
	void *bar0; ///< Pointer to the memory-mapped BAR-region of the controller for mmio

	uint32_t csts; ///< Controller Status Register Value
	uint32_t cap;  ///< Controller Capabilities Register Value
	uint32_t cc;   ///< Controller configuration Register Value

	int timeout_ms;	      ///< Timeout in milliseconds
};

void
nvme_controller_term(struct nvme_controller *ctrlr)
{
	memset(ctrlr, 0, sizeof(*ctrlr));
}

/**
 * Read the CC, CAP, and CSTS controller registers via mmio and store them in given struct
 */
void
nvme_controller_refresh_register_values(struct nvme_controller *ctrlr)
{
	ctrlr->cc = nvme_mmio_cc_read(ctrlr->bar0);
	ctrlr->cap = nvme_mmio_cap_read(ctrlr->bar0);
	ctrlr->csts = nvme_mmio_csts_read(ctrlr->bar0);

	ctrlr->timeout_ms = nvme_reg_cap_get_to(ctrlr->cap) * 500;
}

/**
 * This function is all about host-concerns of pci-scan/bar-map, retrieve properties, this
 * does **not** have anything to do with with CC.EN or similar. Rather it sets up the MMIO
 * based communication channel and retrieves a couple of controller register values: Status,
 * Configuration, and Capabilities. All entities required to do CC.EN=1, but host-level concerns,
 * not NVMe logic per-se.
 */
int
nvme_controller_init(struct nvme_controller *ctrlr, void *bar0)
{
	ctrlr->bar0 = bar0;

	nvme_controller_refresh_register_values(ctrlr);

	return 0;
}
