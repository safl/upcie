struct nvme_qp {
	void *sq;	///< VA-Pointer to DMA-capable memory backing the Submission Queue (SQ)
	void *cq;	///< VA-Pointer to DMA-capable memory backing the Completion Queue (CQ)
	void *sqdb;	///< Pointer to Submission Queue Doorbell Register in bar0
	void *cqdb;	///< Pointer to Completion Queue Doorbell Register in bar0
	uint32_t qid;	///< The admin: queue-id == 0 ; io: queue-id > 0;
	uint16_t depth; ///< Length of the queue-pair
	uint16_t tail;	///< Submissin Queue Tail Pointer
	uint16_t head;	///< Completion Queue Head Pointer
	uint8_t phase;
};

static inline void
nvme_qp_term(struct nvme_qp *qp)
{
	hostmem_dma_free(qp->sq);
	hostmem_dma_free(qp->cq);
}

/**
 * Initialize a queue-pair on the given controller
 */
static inline int
nvme_qp_init(struct nvme_qp *qp, uint32_t qid, uint16_t depth, struct nvme_controller *ctrlr)
{
	uint8_t *bar0 = ctrlr->bar0;

	qp->sqdb = bar0 + 0x1000 + (2 * qid) * ctrlr->dstrd_nbytes;
	qp->cqdb = bar0 + 0x1000 + (2 * qid + 1) * ctrlr->dstrd_nbytes;
	qp->qid = qid;
	qp->tail = 0;
	qp->head = 0;
	qp->depth = depth;
	qp->phase = 1;

	// NOTE: strictly speaking then these values should adhere to the IOSQES
	qp->sq = hostmem_dma_malloc(1024 * 64);
	if (!qp->sq) {
		return -errno;
	}
	memset(qp->sq, 0xFF, 1024 * 64);

	qp->cq = hostmem_dma_malloc(1024 * 64);
	if (!qp->cq) {
		return -errno;
	}
	memset(qp->cq, 0xFF, 1024 * 64);

	return 0;
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
nvme_qp_submit(struct nvme_qp *qp, struct nvme_request_pool *pool, struct nvme_command *cmd,
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
 * Reaps at most a single completion and informs the controller via qp->cqdb
 *
 * @param qp A queue-pair as represented by 'struct nvme_qp'
 * @param cpl Completion when one is reaped
 * @param timeout_us Timeout in microseconds
 *
 * @return Pointer to a valid completion, or NULL on timeout.
 */
static inline struct nvme_completion *
nvme_qp_reap_cpl(struct nvme_qp *qp, int timeout_us)
{
	struct nvme_completion *cq = qp->cq;

	for (int i = 0; i < timeout_us; ++i) {
		volatile struct nvme_completion *cpl = &cq[qp->head];

		if ((cpl->cid < 0xFFFF) && ((cpl->status & 0x1) == qp->phase)) {
			// Valid completion found
			struct nvme_completion *ret = (struct nvme_completion *)cpl;

			// Advance CQ head and toggle phase if wrapping
			qp->head++;
			if (qp->head == qp->depth) {
				qp->head = 0;
				qp->phase ^= 1;
			}

			mmio_write32(qp->cqdb, 0, qp->head);
			return ret;
		}

		usleep(1000);
	}

	return NULL;
}

/**
 * Write the SQ doorbell
 */
static inline void
nvme_qp_sqdb_ring(struct nvme_qp *qp)
{
	mmio_write32(qp->sqdb, 0, qp->tail);
}