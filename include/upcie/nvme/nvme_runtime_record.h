// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * Describing a live runtime to a process that does not have one
 * =============================================================
 *
 * A process holding a controller can let another process use it, but not by
 * handing over its struct. An inventory of what is reachable from
 * struct nvme_controller sorts into four kinds: values that mean the same
 * everywhere, addresses into the heap which are offsets wearing a disguise,
 * addresses into BAR0 which every process computes from its own mapping, and
 * things that must never leave the process that made them, the request pool
 * among them, since its entries carry a pointer belonging to whoever
 * submitted.
 *
 * So what travels is a record of the first kind and offsets of the second, and
 * the receiving process builds its own controller from that plus its own BAR
 * mapping, its own view of the heap and its own request pool. Nothing is
 * rebased and no pointer written by one process is read by another.
 *
 * The record is filled once, when the runtime is built, and is not written
 * again. That is deliberate: the queue identifier space, the heap allocator
 * and the admin queue stay with the process that owns the controller, and a
 * consumer receives a grant naming a queue that has already been created for
 * it. Nothing here needs a lock, because nothing here changes.
 *
 * @file nvme_runtime_record.h
 */

#ifndef __UPCIE_NVME_RUNTIME_RECORD_H
#define __UPCIE_NVME_RUNTIME_RECORD_H

/**
 * Bumped when the layout of the record, or of anything it describes, changes.
 *
 * The record describes queue memory whose layout comes from this library, so a
 * consumer built against a different version cannot be trusted to read it.
 */
#define NVME_RUNTIME_RECORD_VERSION 1U

/**
 * An immutable description of a controller another process has opened
 */
struct nvme_runtime_record {
	uint32_t version;     ///< NVME_RUNTIME_RECORD_VERSION as written
	uint32_t timeout_ms;  ///< Command timeout, derived from CAP.TO
	uint64_t cap;         ///< Controller capabilities as read at open
	uint32_t cc;          ///< Controller configuration as written at open
	uint32_t heap_nbytes; ///< Size of the heap the offsets below refer into
	char bdf[16];         ///< The controller, for a consumer to check it agrees
};

/**
 * A queue created on a consumer's behalf, described in terms it can resolve
 *
 * The offsets are into the heap the record names; the consumer turns them into
 * addresses with its own mapping, and derives the doorbells from its own BAR0.
 */
struct nvme_qpair_grant {
	uint64_t sq_offset;  ///< Submission queue, as a heap offset
	uint64_t cq_offset;  ///< Completion queue, as a heap offset
	uint64_t prp_offset; ///< PRP scratch for the consumer's request pool
	uint32_t qid;        ///< The identifier granted, never zero
	uint16_t depth;      ///< Entries in the queue pair
	uint16_t _rsvd;
};

/**
 * Fill a record from a controller this process opened
 *
 * @param ctrlr An opened controller
 * @param record Pre-allocated record to fill
 *
 * @return 0 on success, negative errno on error
 */
static inline int
nvme_runtime_record_export(const struct nvme_controller *ctrlr, struct nvme_runtime_record *record)
{
	if (!ctrlr || !record || !ctrlr->heap) {
		return -EINVAL;
	}

	memset(record, 0, sizeof(*record));
	record->version = NVME_RUNTIME_RECORD_VERSION;
	record->timeout_ms = (uint32_t)ctrlr->timeout_ms;
	record->cap = nvme_mmio_cap_read(ctrlr->func.bars[0].region);
	record->cc = ctrlr->cc;
	record->heap_nbytes = (uint32_t)ctrlr->heap->memory.size;
	snprintf(record->bdf, sizeof(record->bdf), "%s", ctrlr->func.bdf);

	return 0;
}

/**
 * Describe a queue pair this process created for another to use
 *
 * @param ctrlr The controller the queue belongs to
 * @param qpair A queue pair created with nvme_controller_create_io_qpair
 * @param prps PRP scratch the owner allocated for the consumer, of
 * NVME_REQUEST_POOL_LEN pages
 * @param grant Pre-allocated grant to fill
 *
 * @return 0 on success, negative errno on error
 */
static inline int
nvme_qpair_grant_export(const struct nvme_controller *ctrlr, const struct nvme_qpair *qpair,
			const void *prps, struct nvme_qpair_grant *grant)
{
	const char *base;

	if (!ctrlr || !qpair || !prps || !grant || !ctrlr->heap) {
		return -EINVAL;
	}
	if (!qpair->qid) {
		return -EINVAL; // The admin queue is never granted
	}

	base = (const char *)ctrlr->heap->memory.virt;

	memset(grant, 0, sizeof(*grant));
	grant->sq_offset = (uint64_t)((const char *)qpair->sq - base);
	grant->cq_offset = (uint64_t)((const char *)qpair->cq - base);
	grant->prp_offset = (uint64_t)((const char *)prps - base);
	grant->qid = qpair->qid;
	grant->depth = qpair->depth;

	return 0;
}

/**
 * Build a controller from a record, without touching the device
 *
 * The controller is usable for submitting on granted queues. It does not own
 * the admin queue, the queue identifier space or the heap, so closing it must
 * not go through nvme_controller_close().
 *
 * @param ctrlr Pre-allocated controller to fill
 * @param record A record from nvme_runtime_record_export
 * @param bar0 This process's mapping of BAR0
 * @param heap This process's mapping of the heap the record names
 *
 * @return 0 on success, negative errno on error
 */
static inline int
nvme_runtime_record_import(struct nvme_controller *ctrlr, const struct nvme_runtime_record *record,
			   void *bar0, struct hostmem_heap *heap)
{
	if (!ctrlr || !record || !bar0 || !heap) {
		return -EINVAL;
	}
	if (record->version != NVME_RUNTIME_RECORD_VERSION) {
		UPCIE_DEBUG("FAILED: record version(%u), expected(%u)", record->version,
			    NVME_RUNTIME_RECORD_VERSION);
		return -EPROTO;
	}
	if (heap->memory.size != record->heap_nbytes) {
		UPCIE_DEBUG("FAILED: heap is %zu bytes, record says %u", heap->memory.size,
			    record->heap_nbytes);
		return -EINVAL;
	}

	memset(ctrlr, 0, sizeof(*ctrlr));
	ctrlr->heap = heap;
	ctrlr->timeout_ms = (int)record->timeout_ms;
	ctrlr->cc = record->cc;
	ctrlr->func.bars[0].region = bar0;
	ctrlr->func.bars[0].fd = -1; // Not ours; the owner holds it
	snprintf(ctrlr->func.bdf, sizeof(ctrlr->func.bdf), "%s", record->bdf);

	return 0;
}

/**
 * Build a queue pair from a grant, without creating anything on the device
 *
 * The queue itself already exists; this attaches to it. The request pool is
 * allocated here because it is this process's, and points at the scratch the
 * grant names, since a consumer cannot allocate from the owner's heap. Release
 * it with nvme_qpair_grant_release().
 *
 * @param qpair Pre-allocated queue pair to fill
 * @param grant A grant from nvme_qpair_grant_export
 * @param ctrlr A controller from nvme_runtime_record_import
 *
 * @return 0 on success, negative errno on error
 */
static inline int
nvme_qpair_grant_import(struct nvme_qpair *qpair, const struct nvme_qpair_grant *grant,
			struct nvme_controller *ctrlr)
{
	int dstrd;
	char *base;
	int err;

	if (!qpair || !grant || !ctrlr || !ctrlr->heap || !grant->qid) {
		return -EINVAL;
	}
	if ((grant->sq_offset >= ctrlr->heap->memory.size) ||
	    (grant->cq_offset >= ctrlr->heap->memory.size)) {
		UPCIE_DEBUG("FAILED: grant offsets fall outside the heap");
		return -ERANGE;
	}

	base = (char *)ctrlr->heap->memory.virt;
	dstrd = nvme_reg_cap_get_dstrd(nvme_mmio_cap_read(ctrlr->func.bars[0].region));

	memset(qpair, 0, sizeof(*qpair));
	qpair->heap = ctrlr->heap;
	qpair->qid = grant->qid;
	qpair->depth = grant->depth;
	qpair->sq = base + grant->sq_offset;
	qpair->cq = base + grant->cq_offset;
	qpair->sqdb =
		(char *)ctrlr->func.bars[0].region + 0x1000 + ((2 * grant->qid) << (2 + dstrd));
	qpair->cqdb = (char *)ctrlr->func.bars[0].region + 0x1000 +
		      ((2 * grant->qid + 1) << (2 + dstrd));
	qpair->tail = 0;
	qpair->tail_last_written = UINT16_MAX;
	qpair->head = 0;
	qpair->phase = 1;

	qpair->rpool = (struct nvme_request_pool *)calloc(1, sizeof(*qpair->rpool));
	if (!qpair->rpool) {
		return -errno;
	}
	nvme_request_pool_init(qpair->rpool);

	err = nvme_request_pool_attach_prps(qpair->rpool, ctrlr->heap, grant->prp_offset);
	if (err) {
		free(qpair->rpool);
		qpair->rpool = NULL;
		return err;
	}

	return 0;
}

/**
 * Release what nvme_qpair_grant_import() allocated, leaving the queue alone
 *
 * @param qpair A queue pair from nvme_qpair_grant_import
 */
static inline void
nvme_qpair_grant_release(struct nvme_qpair *qpair)
{
	if (!qpair || !qpair->rpool) {
		return;
	}

	/* The scratch belongs to whoever granted the queue. */
	free(qpair->rpool);
	memset(qpair, 0, sizeof(*qpair));
}

#endif /* __UPCIE_NVME_RUNTIME_RECORD_H */
