struct nvme_command {
	uint8_t opc; ///< opcode
	uint8_t fuse;
	uint16_t cid; ///< command id
	uint32_t nsid;
	uint64_t rsvd2;
	uint64_t mptr;
	uint64_t prp1;
	uint64_t prp2;
	uint32_t cdw10;
	uint32_t cdw11;
	uint32_t cdw12;
	uint32_t cdw13;
	uint32_t cdw14;
	uint32_t cdw15;
};

struct nvme_cmd {
	uint32_t cdw[16];
};

struct nvme_qp {
	void *sq;	///< VA-Pointer to DMA-capable memory backing the Submission Queue (SQ)
	void *cq;	///< VA-Pointer to DMA-capable memory backing the Completion Queue (CQ)
	void *sqdb;	///< Pointer to Submission Queue Doorbell Register in bar0
	void *cqdb;	///< Pointer to Completion Queue Doorbell Register in bar0
	uint16_t tail;	///< Submissin Queue Tail Pointer
	uint16_t head;	///< Completion Queue Head Pointer
	uint16_t depth; ///< Length of the queue-pair
	uint32_t qid;	///< The admin: queue-id == 0 ; io: queue-id > 0;
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
	uint8_t dstrd_nbytes = 1U << (2 + nvme_reg_cap_get_dstrd(ctrlr->cap));
	uint8_t *region = ctrlr->bar0;

	qp->sqdb = region + 0x1000 + (2 * qid) * (1U << (2 + dstrd));
	qp->cqdb = region + 0x1000 + (2 * qid * dstrd);
	qp->sqdb = region + 0x1000 + (2 * qid) * dstrd_nbytes;
	qp->cqdb = region + 0x1000 + (2 * qid + 1) * dstrd_nbytes;
	qp->qid = qid;
	qp->tail = 0;
	qp->head = 0;
	qp->depth = depth;

	// NOTE: strictly speaking then these values should adhere to the IOSQES
	qp->sq = hostmem_dma_malloc(64 * depth);
	if (!qp->sq) {
		return -errno;
	}
	memset(qp->sq, 0, 4096);

	qp->cq = hostmem_dma_malloc(64 * depth);
	if (!qp->cq) {
		return -errno;
	}
	memset(qp->cq, 0, 4096);

	return 0;
}

/**
 * Submits a command to an NVMe submission queue
 *
 * That is, writes it into the submission queue memory and increments the tail-pointer
 *
 * @param mmio    MMIO base address
 * @param cmd     Command to submit
 */
static inline void
nvme_qp_submit(struct nvme_qp *qp, const struct nvme_cmd *cmd)
{
	volatile struct nvme_cmd *sq = qp->sq;

	sq[qp->tail] = *cmd;

	qp->tail++;
	if (qp->tail == qp->depth) {
		qp->tail = 0;
	}
}

/**
 * Write the SQ doorbell
 */
static inline void
nvme_qp_sqdb_ring(struct nvme_qp *qp)
{
	mmio_write32(qp->sqdb, 0, qp->tail);
}