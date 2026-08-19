// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * dmamem constructor: wrap an existing hipmem_heap (LUT translator)
 * =================================================================
 *
 * Closes the "CPU-init + HIP VRAM + iommu=pt/off" cell of the grid by
 * borrowing an already-populated hipmem_heap. The heap has already
 * called hipMalloc for the device VA range,
 * hipMemGetHandleForAddressRange to export as a dma-buf,
 * dmabuf_import_attach + dmabuf_get_lut to enumerate per-device-page PAs into
 * heap->phys_lut. This constructor just points the LUT-translator
 * fields on struct dmamem at the already-populated table and marks the
 * dmamem as wrapping (owned=0) so destroy does not touch the heap's
 * lifetime.
 *
 * The heap's device VA is NOT CPU-mappable; dmem->cpu_va is left NULL,
 * and callers compose PRPs via dmamem_offset_to_iova() with offsets
 * measured from heap->vaddr.
 *
 * @file dmamem_hip.h
 * @version 0.6.0
 */

/**
 * Wrap an existing hipmem_heap as a LUT-translator dmamem.
 *
 * The hipmem_heap must have been initialised via hipmem_heap_init so
 * heap->phys_lut is already populated. No new HIP calls are made here;
 * the dmamem borrows the LUT and every dmamem_offset_to_iova lands as
 * heap->phys_lut[offset >> shift] + intra-page offset.
 *
 * @param dmem  Pre-allocated dmamem descriptor to fill.
 * @param heap  Initialised hipmem_heap to borrow.
 *
 * @return 0 on success, negative errno on error.
 */
static inline int
dmamem_from_hip_lut(struct dmamem *dmem, struct hipmem_heap *heap)
{
	int shift;

	if (!dmem || !heap || !heap->phys_lut || !heap->config) {
		return -EINVAL;
	}

	shift = dmamem_lut_pagesize_shift(heap->config->device_pagesize);
	if (shift < 0) {
		UPCIE_DEBUG("FAILED: unsupported device_pagesize(%zu)",
			    (size_t)heap->config->device_pagesize);
		return -EINVAL;
	}

	memset(dmem, 0, sizeof(*dmem));
	dmem->fd = -1;
	dmem->base_va = (void *)(uintptr_t)heap->vaddr;
	dmem->cpu_va = NULL;
	dmem->size = heap->size;
	dmem->backing = DMAMEM_BACKING_HIPMEM;
	dmem->translator = DMAMEM_XLATE_LUT;
	dmem->phys_lut = heap->phys_lut;
	dmem->hugepgsz = heap->config->device_pagesize;
	dmem->hugepgsz_shift = shift;
	dmem->owned = 0;

	return 0;
}

/**
 * Discover one chunk's address from HIP, for a dmamem_registry.
 *
 * Exports the chunk as a dma-buf, attaches it, enumerates the host-page
 * addresses and verifies they are contiguous, which is the large-page assumption the chunk model rests on.
 *
 * `ctx` is the `struct hipmem_config *` for the device the registry covers.
 *
 * @return 0 on success, negative errno on failure. -EOPNOTSUPP when a chunk
 *         turns out not to be contiguous.
 */
static inline int
dmamem_hip_registry_populate(void *ctx, uint64_t chunk_va, size_t granularity,
			      uint64_t *phys_base_out, struct dmabuf *attach_out)
{
	struct hipmem_config *config = ctx;
	const size_t pagesize = (size_t)config->pagesize;
	const size_t nphys = granularity >> config->pagesize_shift;
	struct dmabuf attach = {0};
	uint64_t *tmp = NULL;
	int dmabuf_fd = -1;
	int err;
	hipError_t cr;

	tmp = calloc(nphys, sizeof(*tmp));
	if (!tmp) {
		return -ENOMEM;
	}

	cr = hipMemGetHandleForAddressRange(&dmabuf_fd, (hipDeviceptr_t)chunk_va, granularity,
					   hipMemRangeHandleTypeDmaBufFd, 0);
	if (cr != hipSuccess) {
		UPCIE_DEBUG("FAILED: hipMemGetHandleForAddressRange(0x%" PRIx64 ", %zu), cr: %d",
			    chunk_va, granularity, cr);
		err = -EIO;
		goto err_free;
	}

	/* NOTE: EXPERIMENTAL dependency, see <upcie/experimental/dmabuf_import.h> */
	err = dmabuf_import_attach(dmabuf_fd, &attach);
	if (err) {
		UPCIE_DEBUG("FAILED: dmabuf_import_attach(), err: %d", err);
		close(dmabuf_fd);
		goto err_free;
	}

	err = dmabuf_get_lut(&attach, nphys, tmp, (int)pagesize);
	if (err) {
		UPCIE_DEBUG("FAILED: dmabuf_get_lut(), err: %d", err);
		goto err_detach;
	}

	for (size_t i = 1; i < nphys; ++i) {
		if (tmp[i] != tmp[0] + i * pagesize) {
			UPCIE_DEBUG("FAILED: chunk not contiguous at i=%zu; phys[0]=0x%" PRIx64
				    " phys[%zu]=0x%" PRIx64,
				    i, tmp[0], i, tmp[i]);
			err = -EOPNOTSUPP;
			goto err_detach;
		}
	}

	*phys_base_out = tmp[0];
	*attach_out = attach;
	free(tmp);

	return 0;

err_detach:
	dmabuf_import_detach(&attach);
err_free:
	free(tmp);

	return err;
}

/**
 * Release one chunk discovered by dmamem_hip_registry_populate().
 */
static inline void
dmamem_hip_registry_release(void *UPCIE_UNUSED(ctx), struct dmabuf *attach)
{
	dmabuf_import_detach(attach);
}

/**
 * Wrap a registry as a dmamem, seeding it with the heap.
 *
 * The heap is adopted rather than rediscovered, since it enumerated its own
 * addresses at init, and it then resolves through the same translator as
 * every registered buffer. That is what lets one dmamem serve both, so the
 * command paths need no notion of where a buffer came from.
 *
 * The registry must be initialised with the device's alloc_granularity and
 * dmamem_hip_registry_populate(); the caller keeps ownership of both it and
 * the heap, and both must outlive the dmamem.
 *
 * @return 0 on success, negative errno on failure.
 */
static inline int
dmamem_from_hip_registry(struct dmamem *dmem, struct dmamem_registry *registry,
			  struct hipmem_heap *heap)
{
	int err;

	if (!dmem || !registry || !heap || !heap->phys_lut || !heap->config) {
		return -EINVAL;
	}

	err = dmamem_registry_adopt(registry, (void *)(uintptr_t)heap->vaddr, heap->size,
				    heap->phys_lut, heap->config->device_pagesize_shift, NULL);
	if (err) {
		UPCIE_DEBUG("FAILED: dmamem_registry_adopt(heap), err: %d", err);
		return err;
	}

	memset(dmem, 0, sizeof(*dmem));
	dmem->fd = -1;
	dmem->base_va = (void *)(uintptr_t)heap->vaddr;
	dmem->cpu_va = NULL;
	dmem->size = heap->size;
	dmem->backing = DMAMEM_BACKING_HIPMEM;
	dmem->translator = DMAMEM_XLATE_REGISTRY;
	dmem->registry = registry;
	dmem->owned = 0;

	return 0;
}
