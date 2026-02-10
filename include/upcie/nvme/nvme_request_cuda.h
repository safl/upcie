// SPDX-License-Identifier: BSD-3-Clause


/**
 * CUDA NVMe Request Extension
 * ===========================
 *
 * This header extends the functionality defined in the uPCIe NVMe Request header
 * `upcie/nvme/nvme_request.h` with a CUDA dependent PRP preparation function.
 */

/**
 * Prepare the PRP list for a command with a contiguous CUDA data buffer.
 *
 * This function initializes the Physical Region Page (PRP) entries in the given NVMe command
 * (`cmd`) using the provided request and a contiguous data buffer (in VA-space).
 * It sets up the PRP1 and PRP2 fields in the command to describe the physical memory backing the
 * `data` buffer, allowing the NVMe controller to access the buffer during command execution.
 *
 * Caveats
 * -------
 *
 * - Assumes that the memory backing `dbuf` in `heap` is physically contiguous.
 * - Does *not* support PRP list chaining; only a single list page is constructed.
 *
 * @param request Pointer to the NVMe request context used for tracking and metadata.
 * @param heap Pointer to the CUDA memory heap that dbuf is allocated within.
 * @param dbuf Pointer to the contiguous data buffer to be described by PRPs.
 * @param dbuf_nbytes Size in bytes of the data buffer.
 * @param cmd Pointer to the NVMe command to be prepared with PRP entries.
 */
static inline void
nvme_request_prep_command_prps_contig_cuda(struct nvme_request *request, struct cudamem_heap *heap,
                                           void *dbuf, size_t dbuf_nbytes, struct nvme_command *cmd)
{
	const uint64_t npages = dbuf_nbytes >> heap->pagesize_shift;
	const uint64_t pagesize = heap->pagesize;

	/* Chaining is not supported, thus assert that the given dbuf fits. */
	assert(npages <= 1 + 8192);

	cmd->prp1 = cudamem_heap_block_vtp(heap, dbuf);

	if (npages == 1) {
		return;
	} else if (npages == 2) {
		cmd->prp2 = cmd->prp1 + pagesize;
	} else {
		uint64_t *prp_list = request->prp;

		cmd->prp2 = request->prp_addr;
		for (uint64_t i = 1; i < npages; ++i) {
			prp_list[i - 1] = cmd->prp1 + (i << heap->pagesize_shift);
		}
	}
}