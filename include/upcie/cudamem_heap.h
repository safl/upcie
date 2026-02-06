// SPDX-License-Identifier: BSD-3-Clause

/**
 * Heap-based memory allocator backed by the CUDA driver
 * =====================================================
 *
 * This heap implementation uses the CUDA driver to pre-allocate memory and
 * the dma-buf interface to get the physical addresses.
 * While the heap memory is allocated on the GPU, the freelist is maintained in
 * host memory. This avoids segfaults at the cost of slower alloc/free.
 *
 * Caveat: Hardware requirements
 * -----------------------------
 * This requires a GPU with support for PCIe P2P DMA and a large BAR1 memory
 * region. To find the size of the BAR1 memory run:
 *
 *	nvidia-smi -q -d memory
 *
 */

/**
 * A block in the freelist
 *
 * Each block represents a memory segment which can either be free or allocated.
 * The blocks are produced by cudamem_heap_block_alloc() or
 * cudamem_heap_block_alloc_aligned().
 */
struct cudamem_heap_block {
	uint64_t vaddr;				///< Virtual address of the underlying memory segment (stored as integer to avoid segfaults)
	size_t size;				///< Size of the underlying memory segment
	int free;				///< If free == 1, the block is free and can be allocated
	struct cudamem_heap_block *next;	///< Pointer to the next element in the freelist
};

/**
 * A pre-allocated heap providing memory for a buffer-allocator
 */
struct cudamem_heap {
	uint64_t vaddr;				///< Virtual address of the beginning of the heap (stored as integer to avoid segfaults)
	struct cudamem_heap_block *freelist;	///< Pointers to description of free memory in the heap
	struct dmabuf dmabuf;			///< Representation of a dma-buf
	size_t size;				///< Size of the heap
	size_t pagesize;			///< Size of a physical page
	size_t pagesize_shift;                  ///< Size of a physical page as a power of two, pagesize == 1 << pagesize_shift
	size_t nphys;				///< Number of physical pages backing 'memory'
	uint64_t *phys_lut;			///< An array of physical addresses; one for each page in 'memory'
};

/**
 * Print information about the given heap and each block in its freelist
 */
static inline int
cudamem_heap_pp(struct cudamem_heap *heap)
{
	int wrtn = 0;

	wrtn += printf("cudamem_heap:");

	if (!heap) {
		wrtn += printf(" ~\n");
		return 0;
	}

	wrtn += printf("\n");

	wrtn += printf("  size: '%zu'\n", heap->size);
	wrtn += printf("  pagesize: '%zu'\n", heap->pagesize);
	wrtn += printf("  nphys: '%zu'\n", heap->nphys);
	wrtn += printf("  phys:\n");
	for (size_t i = 0; i < heap->nphys; ++i) {
		wrtn += printf("  - 0x%" PRIx64 "\n", heap->phys_lut[i]);
	}

	wrtn += printf("  freelist:\n");
	for (struct cudamem_heap_block *block = heap->freelist; block; block = block->next) {
		wrtn += printf("  - {vaddr: 0x%" PRIx64 ", size: %zu, free: %d}\n", block->vaddr, block->size, block->free);
	}

	return wrtn;
}

/**
 * Free every block in the freelist
 */
static inline void
cudamem_heap_empty_freelist(struct cudamem_heap_block *block)
{
	struct cudamem_heap_block *next;

	while(block) {
		next = block->next;
		free(block);
		block = next;
	}
}

/**
 * Terminate the given heap, freeing the underlying memory and emptying the freelist
 */
static inline void
cudamem_heap_term(struct cudamem_heap *heap)
{
	if (!heap) {
		return;
	}

	dmabuf_detach(&heap->dmabuf);
	cudamem_heap_empty_freelist(heap->freelist);
	free(heap->phys_lut);
	cuMemFree((CUdeviceptr)heap->vaddr);
}


/**
 * Initialize the given heap
 *
 * - Pre-allocate a va-space of 'size' bytes backend by GPU page(s)
 * - Setup the LUT / physical address for the va-space
 *
 * NOTE: Set up CUDA Driver (cuInit()) and CUDA Context (cuCtxCreate())
 * before calling this function.
 *
 */
static inline int
cudamem_heap_init(struct cudamem_heap *heap, size_t size)
{
	CUdeviceptr vaddr;
	int dmabuf_fd, err;

	if (!heap) {
		return -EINVAL;
	}

	memset(heap, 0, sizeof(*heap));

	err = cuMemAlloc(&vaddr, size);
	if (err) {
		UPCIE_DEBUG("FAILED: cuMemAlloc(heap), err: %d", err);
		return err;
	}

	err = cuMemGetHandleForAddressRange(&dmabuf_fd, vaddr, size, CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD, 0);
	if (err) {
		UPCIE_DEBUG("FAILED: cuMemGetHandleForAddressRange(heap), err: %d", err);
		goto error;
	}

	heap->vaddr = (uint64_t) vaddr;
	heap->size = size;
	heap->pagesize_shift = 16;
	heap->pagesize = 1 << heap->pagesize_shift; // 64K

	// Initialize a single free block spanning the entire heap
	heap->freelist = malloc(sizeof(struct cudamem_heap_block));
	if (!heap->freelist) {
		err = -errno;
		UPCIE_DEBUG("FAILED: malloc(freelist), errno: %d", err);
		goto error;
	}
	heap->freelist->vaddr = heap->vaddr;
	heap->freelist->size = heap->size;
	heap->freelist->free = 1;
	heap->freelist->next = NULL;

	err = dmabuf_attach(dmabuf_fd, &heap->dmabuf);
	if (err) {
		UPCIE_DEBUG("FAILED: dmabuf_attach(), err: %d", err);
		goto error;
	}

	// Setup the LUT
	heap->nphys = heap->size / heap->pagesize;
	heap->phys_lut = calloc(heap->nphys, sizeof(uint64_t));
	if (!heap->phys_lut) {
		err = -errno;
		UPCIE_DEBUG("FAILED: calloc(phys_lut), errno: %d", err);
		goto error;
	}
	err = dmabuf_get_lut(&heap->dmabuf, heap->nphys, heap->phys_lut, heap->pagesize);
	if (err) {
		UPCIE_DEBUG("FAILED: dmabuf_get_lut(), err: %d", err);
		goto error;
	}

	return 0;

error:
	free(heap->freelist);
	free(heap->phys_lut);
	cuMemFree(vaddr);
	return err;
}

/**
 * Free a block allocated on the given heap
 *
 * Only blocks allocated by cudamem_heap_block_alloc() or
 * cudamem_heap_block_alloc_aligned() can be freed.
 */
static inline void
cudamem_heap_block_free(struct cudamem_heap *heap, void *ptr)
{
	struct cudamem_heap_block *block;
	struct cudamem_heap_block *tmp;
	uint64_t vaddr;

	if (!ptr) {
		return;
	}

	vaddr = (uint64_t) ptr;

	block = heap->freelist;
	while(block && block->next) {
		if (block->vaddr == vaddr) {
			block->free = 1;
		} else if (block->next->vaddr == vaddr) {
			block->next->free = 1;
		}
		if (block->free && block->next->free) {
			tmp = block->next;
			block->size += tmp->size;
			block->next = tmp->next;
			free(tmp);
		} else {
			block = block->next;
		}
	}
}

/**
 * Allocate a block on the given heap with a custom alignment
 */
static inline void *
cudamem_heap_block_alloc_aligned(struct cudamem_heap *heap, size_t size, size_t alignment)
{
	struct cudamem_heap_block *block = heap->freelist;

	size = (size + alignment - 1) & ~(alignment - 1);

	while (block) {
		if (block->free && block->size >= size) {
			size_t remaining = block->size - size;
			if (remaining) {
				struct cudamem_heap_block *newblock = malloc(sizeof(struct cudamem_heap_block));
				if (!newblock) {
					UPCIE_DEBUG("FAILED: malloc(newblock), errno: %d", errno);
					return NULL;
				}

				newblock->vaddr = block->vaddr + size;
				newblock->free = 1;
				newblock->size = remaining;
				newblock->next = block->next;

				block->next = newblock;
				block->size = size;

			}

			block->free = 0;
			return (void *) block->vaddr;
		}
		block = block->next;
	}

	errno = ENOMEM;
	return NULL;
}

/**
 * Allocate a block on the given heap aligned to the GPU pagesize (64K)
 */
static inline void *
cudamem_heap_block_alloc(struct cudamem_heap *heap, size_t size)
{
	return cudamem_heap_block_alloc_aligned(heap, size, heap->pagesize);
}

/**
 * Calculate the physical address of a block on the given heap
 */
static inline int
cudamem_heap_block_virt_to_phys(struct cudamem_heap *heap, void *virt, uint64_t *phys)
{
	size_t offset, page_idx, in_page_offset;
	uint64_t vaddr = (uint64_t) virt;

	if (!heap || !heap->phys_lut || !virt || !phys) {
		return -EINVAL;
	}

	if (vaddr < heap->vaddr || vaddr > heap->vaddr + heap->size) {
		return -EINVAL;
	}

	// Compute byte offset from base of heap
	offset = vaddr - heap->vaddr;

	// Determine which page this address falls into
	page_idx = offset / heap->pagesize;

	if (page_idx >= heap->nphys) {
		return -EINVAL;
	}

	// Offset within that page
	in_page_offset = offset % heap->pagesize;

	*phys = heap->phys_lut[page_idx] + in_page_offset;

	return 0;
}

/**
 * Calculate the physical address of a block on the given heap
 *
 * Same as cudamem_buffer_virt_to_phys() but without any error-handling, thus return the phys
 * address instead of error
 */
static inline uint64_t
cudamem_heap_block_vtp(struct cudamem_heap *heap, void *virt)
{
	size_t offset, page_idx, in_page_offset;
	uint64_t vaddr = (uint64_t) virt;

	// Compute byte offset from base of heap
	offset = vaddr - heap->vaddr;

	// Determine which page this address falls into
	page_idx = offset / heap->pagesize;

	// Offset within that page
	in_page_offset = offset % heap->pagesize;

	return heap->phys_lut[page_idx] + in_page_offset;
}
