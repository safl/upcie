// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * NVMe Queue Pair Abstraction
 * ===========================
 *
 * This header defines a minimal software abstraction for managing NVMe queue pairs (SQ/CQ) in a
 * user-space NVMe driver context. It provides basic functionality for queue setup, submission,
 * completion handling, and doorbell notification.
 *
 * A queue pair is represented by 'struct nvme_qpair', which includes memory-mapped pointers to
 * the submission and completion queues, doorbell registers, and associated tracking state (head,
 * tail, phase).
 *
 * Key functions include:
 *
 * nvme_qpair_init():      Initializes a queue pair and allocates DMA memory for SQ/CQ.
 * nvme_qpair_term():      Frees resources associated with a queue pair.
 * nvme_qpair_reap_cpl():  Polls the CQ for a completion, updates head/phase, and rings CQ doorbell.
 * nvme_qpair_sqdb_ring(): Notifies the controller by ringing the SQ doorbell.
 * nvme_qpair_submit():    Enqueues a command into the SQ and assigns a CID.
 * nvme_qpair_submit_sync(): Submits a command and waits synchronously for its completion.
 *
 * See also: nvme_qid.h for queue ID (qid) management.
 *
 * @file nvme_qpair.h
 * @version 0.1.1
 */

struct nvme_qpair {
	void *sq;	///< VA-Pointer to DMA-capable memory backing the Submission Queue (SQ)
	void *cq;	///< VA-Pointer to DMA-capable memory backing the Completion Queue (CQ)
	void *sqdb;	///< Pointer to Submission Queue Doorbell Register in bar0
	void *cqdb;	///< Pointer to Completion Queue Doorbell Register in bar0
	uint32_t qid;	///< The admin: queue-id == 0 ; io: queue-id > 0;
	uint16_t depth; ///< Length of the queue-pair
	uint16_t tail;	///< Submissin Queue Tail Pointer
	uint16_t head;	///< Completion Queue Head Pointer
	uint8_t phase;
	struct hostmem_heap *heap; ///< For allocation / free of DMA-capable SQ/CQ entries
};

static inline void
nvme_qpair_term(struct nvme_qpair *qp)
{
	hostmem_dma_free(qp->heap, qp->sq);
	hostmem_dma_free(qp->heap, qp->cq);
}

/**
 * Initialize a queue-pair on the given controller
 */
static inline int
nvme_qpair_init(struct nvme_qpair *qp, uint32_t qid, uint16_t depth, uint8_t *bar0, struct hostmem_heap *heap)
{
	int dstrd = nvme_reg_cap_get_dstrd(nvme_mmio_cap_read(bar0));
	size_t nbytes = 1024 * 64;

	qp->heap = heap;
	qp->sqdb = bar0 + 0x1000 + ((2 * qid) << dstrd);
	qp->cqdb = bar0 + 0x1000 + ((2 * qid + 1) << dstrd);
	qp->qid = qid;
	qp->tail = 0;
	qp->head = 0;
	qp->depth = depth;
	qp->phase = 1;

	// qp->sq = hostmem_dma_malloc(1024 * ctrlr->iosqes_nbytes);
	qp->sq = hostmem_dma_malloc(qp->heap, nbytes);
	if (!qp->sq) {
		printf("FAILED: hostmem_dma_malloc(sq); errno(%d)\n", errno);
		return -errno;
	}
	memset(qp->sq, 0xFF, nbytes);

	qp->cq = hostmem_dma_malloc(qp->heap, nbytes);
	if (!qp->cq) {
		printf("FAILED: hostmem_dma_malloc(cq); errno(%d)\n", errno);
		return -errno;
	}
	memset(qp->cq, 0xFF, nbytes);

	return 0;
}

/**
 * Reaps at most a single completion and informs the controller via qp->cqdb
 *
 * @param qp A queue-pair as represented by 'struct nvme_qp'
 * @param cpl Completion when one is reaped
 * @param timeout_us Timeout in microseconds
 *
 * @return Pointer to a valid completion, or NULL on timeout.
 */
static inline int
nvme_qpair_reap_cpl(struct nvme_qpair *qp, int timeout_us, struct nvme_completion *cpl)
{
	struct nvme_completion *cq = qp->cq;

	for (int i = 0; i < timeout_us; ++i) {
		struct nvme_completion *cqe = &cq[qp->head];

		if ((cqe->cid < 0xFFFF) && ((cqe->status & 0x1) == qp->phase)) {
			*cpl = *cqe;

			// Advance CQ head and toggle phase if wrapping
			qp->head++;
			if (qp->head == qp->depth) {
				qp->head = 0;
				qp->phase ^= 1;
			}

			mmio_write32(qp->cqdb, 0, qp->head);
			return 0;
		}

		usleep(1000);
	}

	return -EAGAIN;
}

/**
 * Write the SQ doorbell
 */
static inline void
nvme_qpair_sqdb_ring(struct nvme_qpair *qp)
{
	mmio_write32(qp->sqdb, 0, qp->tail);
}

/**
 * Submits a command to an NVMe submission queue
 *
 * That is, writes it into the submission queue memory and increments the tail-pointer. Note that
 * 'cmd' will be modified by assignment of command-identifier.
 *
 * @param qp The queue-pair
 * @param cmd Command to submit
 * @param pool Request pool used to allocate a new CID
 * @param user Optional opaque pointer returned on completion
 *
 * @return On success 0 is returned. On error then negative errno is set to indicate the error.
 */
static inline int
nvme_qpair_submit(struct nvme_qpair *qp, struct nvme_request_pool *pool, struct nvme_command *cmd,
		  void *user)
{
	volatile struct nvme_command *sq = qp->sq;
	struct nvme_request *req;

	req = nvme_request_alloc(pool);
	if (!req) {
		return -ENOSPC;
	}

	req->user = user;
	cmd->cid = req->cid;
	sq[qp->tail] = *cmd;

	qp->tail = (qp->tail + 1) % qp->depth;

	return 0;
}

/**
 * Submits the given command, notifies the controller, and waits for the completion
 */
static inline int
nvme_qpair_submit_sync(struct nvme_qpair *qp, struct nvme_request_pool *qprp,
		       struct nvme_command *cmd, int timeout_us, struct nvme_completion *cpl)
{
	int err;

	err = nvme_qpair_submit(qp, qprp, cmd, NULL);
	if (err) {
		return -err;
	}
	nvme_qpair_sqdb_ring(qp);

	err = nvme_qpair_reap_cpl(qp, timeout_us, cpl);
	if (err) {
		return -err;
	}

	if (cpl->status & 0x1FE) {
		err = -EIO;
	}

	return err;
}
