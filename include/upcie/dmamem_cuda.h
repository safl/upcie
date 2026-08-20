// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * dmamem constructor: wrap an existing cudamem_heap (LUT translator)
 * ==================================================================
 *
 * Closes the "CPU-init + CUDA VRAM + iommu=pt/off" cell of the grid by
 * borrowing an already-populated cudamem_heap. The heap has already
 * done the heavy lifting: cuMemAlloc for the device VA range,
 * cuMemGetHandleForAddressRange to export the range as a dma-buf,
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
 * @file dmamem_cuda.h
 * @version 0.6.0
 */

/**
 * Wrap an existing cudamem_heap as a LUT-translator dmamem.
 *
 * The cudamem_heap must have been initialised via cudamem_heap_init so
 * heap->phys_lut is already populated. No new CUDA calls are made
 * here; the dmamem borrows the LUT and every dmamem_offset_to_iova
 * lands as heap->phys_lut[offset >> shift] + intra-page offset.
 *
 * @param dmem  Pre-allocated dmamem descriptor to fill.
 * @param heap  Initialised cudamem_heap to borrow.
 *
 * @return 0 on success, negative errno on error.
 */
static inline int
dmamem_from_cuda_lut(struct dmamem *dmem, struct cudamem_heap *heap)
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
	dmem->backing = DMAMEM_BACKING_CUDAMEM;
	dmem->translator = DMAMEM_XLATE_LUT;
	dmem->phys_lut = heap->phys_lut;
	dmem->hugepgsz = heap->config->device_pagesize;
	dmem->hugepgsz_shift = shift;
	dmem->owned = 0;

	return 0;
}

/**
 * Granularity for a CUDA-backed dmamem_registry.
 *
 * Matches the device's alloc_granularity, the BAR1 large-page size, which is
 * 2 MiB on the parts uPCIe targets. Sizing the LUT by it costs 1 GiB of
 * reservation over a 48-bit address space.
 */
#define DMAMEM_CUDA_REGISTRY_GRANULARITY (2UL << 20)

/**
 * Recover the CUDA allocation that `va` falls inside, for a dmamem_registry.
 *
 * An export describes an allocation, not a range, so a registration has to be
 * placed at its offset within one. `ctx` is unused; the runtime knows.
 *
 * @return 0 on success, -EINVAL when `va` is not a known device address.
 */
static inline int
dmamem_cuda_registry_range(void *UPCIE_UNUSED(ctx), uint64_t va, uint64_t *base_out,
			  size_t *size_out)
{
	CUdeviceptr b = 0;
	CUresult cr = cuMemGetAddressRange(&b, size_out, (CUdeviceptr)va);

	if (cr != CUDA_SUCCESS) {
		UPCIE_DEBUG("FAILED: cuMemGetAddressRange(0x%" PRIx64 "), cr: %d", va, cr);
		return -EINVAL;
	}
	*base_out = (uint64_t)b;

	return 0;
}

/**
 * Make one CUDA allocation addressable, for a dmamem_registry.
 *
 * Exports the whole allocation as a dma-buf once, attaches it, and summarises
 * the scatter list into one address per granule. Exporting the allocation
 * rather than the registered range is not an optimisation: ROCm discards the
 * range arguments and returns the whole buffer object regardless, so a
 * per-range export resolves a sub-range to the base of the allocation. CUDA
 * honours the range, so the same shape is correct there too. See
 * `tools/upcie_dmabuf_probe_{cuda,hip}` for the measurements.
 *
 * @return 0 on success, negative errno on failure. -EOPNOTSUPP when a granule
 *         turns out not to be contiguous.
 */
static inline int
dmamem_cuda_registry_populate(void *UPCIE_UNUSED(ctx), uint64_t base, size_t size,
			     uint64_t granularity, uint64_t *lut_out, size_t nlut,
			     struct dmabuf *attach_out)
{
	struct dmabuf attach = {0};
	int dmabuf_fd = -1;
	int err;
	CUresult cr;

	cr = cuMemGetHandleForAddressRange(&dmabuf_fd, (CUdeviceptr)base, size,
					   CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD, 0);
	if (cr != CUDA_SUCCESS) {
		UPCIE_DEBUG("FAILED: cuMemGetHandleForAddressRange(0x%" PRIx64 ", %zu), cr: %d",
			    base, size, cr);
		return -EIO;
	}

	/* NOTE: EXPERIMENTAL dependency, see <upcie/experimental/dmabuf_import.h> */
	err = dmabuf_import_attach(dmabuf_fd, &attach);
	if (err) {
		UPCIE_DEBUG("FAILED: dmabuf_import_attach(), err: %d", err);
		close(dmabuf_fd);
		return err;
	}

	err = dmabuf_get_granule_lut(&attach, lut_out, nlut, granularity);
	if (err) {
		UPCIE_DEBUG("FAILED: dmabuf_get_granule_lut(), err: %d", err);
		dmabuf_import_detach(&attach);
		return err;
	}

	*attach_out = attach;

	return 0;
}

/**
 * Release one allocation made addressable by dmamem_cuda_registry_populate().
 */
static inline void
dmamem_cuda_registry_release(void *UPCIE_UNUSED(ctx), struct dmabuf *attach)
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
 * The registry must be initialised with dmamem_cuda_registry_range() and
 * dmamem_cuda_registry_populate(); the caller keeps ownership of both it and
 * the heap, and both must outlive the dmamem.
 *
 * @return 0 on success, negative errno on failure.
 */
static inline int
dmamem_from_cuda_registry(struct dmamem *dmem, struct dmamem_registry *registry,
			  struct cudamem_heap *heap)
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
	dmem->backing = DMAMEM_BACKING_CUDAMEM;
	dmem->translator = DMAMEM_XLATE_REGISTRY;
	dmem->registry = registry;
	dmem->owned = 0;

	return 0;
}
