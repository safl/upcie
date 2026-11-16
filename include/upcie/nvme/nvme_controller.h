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
 * @version 0.12.0
 */

/**
 * Admin command timeout in milliseconds, the default admin_timeout of the Linux NVMe driver
 */
#define NVME_ADMIN_TIMEOUT_MS 60000

/**
 * What a controller is, as another process using it needs to know
 *
 * Values only, no pointers, so it can be handed to another process as it is.
 * A process given it builds a controller of its own around it, with its own
 * BAR0 mapping, heap view and admin-less queues.
 */
struct nvme_controller_base {
	uint64_t cap;              ///< Controller Capabilities, as read at open
	uint32_t cc;               ///< Controller Configuration, as written at enable
	uint32_t timeout_ms;       ///< CSTS.RDY transition timeout in milliseconds, from CAP.TO
	uint32_t admin_timeout_ms; ///< Admin command timeout in milliseconds
	uint16_t nsqa;             ///< I/O Submission Queues allocated, by Number of Queues
	uint16_t ncqa;             ///< I/O Completion Queues allocated, by Number of Queues
};

UPCIE_STATIC_ASSERT(sizeof(struct nvme_controller_base) == 24,
		    "nvme_controller_base changed size; another process reads it as it is")

/**
 * This is one way of combining the various components needed
 */
struct nvme_controller {
	struct pci_func func;                 ///< The PCIe function and mapped bars
	struct nvme_qpair aq;                 ///< Admin qpair
	uint64_t qids[NVME_QID_BITMAP_WORDS]; ///< Allocation status of IO queues

	uint32_t csts; ///< Controller Status Register Value

	struct nvme_controller_base base; ///< What another process using it needs to know
};

/**
 * Sends a Delete I/O Completion Queue admin command for `qid`
 *
 * @param ctrlr Pointer to a pre-allocated NVMe controller
 * @param qid The identifier of the completion-queue to delete
 *
 * @return 0 on success, negative errno on error.
 */
static inline int
nvme_controller_delete_io_cq(struct nvme_controller *ctrlr, uint16_t qid)
{
	struct nvme_command cmd = {0};
	struct nvme_completion cpl = {0};

	cmd.opc = 0x4; ///< Delete I/O Completion Queue
	cmd.cdw10 = qid;

	return nvme_qpair_submit_sync(&ctrlr->aq, &cmd, ctrlr->base.admin_timeout_ms, &cpl);
}

/**
 * Sends Set Features Number of Queues for the most I/O queues and keeps the counts allocated
 *
 * The first successful Set Features of Number of Queues after a Controller Level Reset allocates
 * the I/O queues, and the allocation holds until the next reset. The controller can allocate
 * fewer queues than requested, and an I/O queue identifier above the count it allocated is
 * invalid. So a controller is sent this after enable, before any I/O queue is created.
 *
 * A controller that fails the Set Features is asked Get Features Number of Queues instead, and
 * the counts kept are the allocation it made without a request.
 *
 * @param ctrlr Pointer to an enabled NVMe controller with no I/O queues
 *
 * @return 0 on success, negative errno on error.
 */
static inline int
nvme_controller_set_nqueues(struct nvme_controller *ctrlr)
{
	struct nvme_command cmd = {0};
	struct nvme_completion cpl = {0};
	uint32_t nsqa, ncqa;
	int err;

	cmd.opc = 0x9;          ///< Set Features
	cmd.cdw10 = 0x7;        ///< Number of Queues
	cmd.cdw11 = 0xFFFEFFFE; ///< NCQR and NSQR, 0's based, the largest a request can be

	err = nvme_qpair_submit_sync(&ctrlr->aq, &cmd, ctrlr->base.admin_timeout_ms, &cpl);
	if (err) {
		UPCIE_DEBUG("INFO: Set Features Number of Queues; err(%d) status(0x%x)", err,
			    cpl.status);

		memset(&cmd, 0, sizeof(cmd));
		memset(&cpl, 0, sizeof(cpl));
		cmd.opc = 0xA;   ///< Get Features
		cmd.cdw10 = 0x7; ///< Number of Queues, current value

		err = nvme_qpair_submit_sync(&ctrlr->aq, &cmd, ctrlr->base.admin_timeout_ms, &cpl);
		if (err) {
			UPCIE_DEBUG("FAILED: Get Features Number of Queues; err(%d)", err);
			return err;
		}
	}

	// NSQA and NCQA are 0's based, and 0xFFFF is not a count a controller can allocate
	nsqa = cpl.cdw0 & 0xFFFF;
	ncqa = cpl.cdw0 >> 16;
	ctrlr->base.nsqa = (nsqa < 0xFFFE ? nsqa : 0xFFFE) + 1;
	ctrlr->base.ncqa = (ncqa < 0xFFFE ? ncqa : 0xFFFE) + 1;

	return 0;
}

/**
 * Allocates the lowest free I/O queue identifier that the controller allocated a queue pair for
 *
 * @param ctrlr Pointer to an NVMe controller sent nvme_controller_set_nqueues()
 *
 * @return The identifier on success; -ENOMEM when every identifier from 1 to the smaller of
 *         `nsqa` and `ncqa` is allocated.
 */
static inline int
nvme_controller_qid_alloc(struct nvme_controller *ctrlr)
{
	uint32_t nqpairs =
		ctrlr->base.nsqa < ctrlr->base.ncqa ? ctrlr->base.nsqa : ctrlr->base.ncqa;
	int qid;

	qid = nvme_qid_find_free(ctrlr->qids);
	if (qid < 1 || (uint32_t)qid > nqpairs) {
		return -ENOMEM;
	}

	return nvme_qid_alloc(ctrlr->qids, qid) ? -ENOMEM : qid;
}
