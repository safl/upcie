// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * Hugepage-backed malloc-like allocator for DMA in userspace
 * ===========================================================
 *
 * This header provides a minimal, header-only allocator for use in user-space drivers
 * requiring DMA-capable memory. Allocations are backed by hugepages and are guaranteed
 * to be contiguous in both virtual and physical address space — up to hugepage granularity.
 *
 * All allocations are managed through a global `hostmem_heap` instance:
 *
 *     struct hostmem_heap g_hostmem_dma;
 *
 * Interface
 * ---------
 *
 *  - int hostmem_dma_init(size_t size);
 *    Initialize the allocator and reserve a contiguous region of hugepage-backed memory.
 *
 *  - void *hostmem_dma_malloc(size_t size);
 *    Allocate a block of memory of the given size.
 *
 *  - void hostmem_dma_free(void *ptr);
 *    Frees a block previously returned by hostmem_dma_malloc().
 *
 *  - uint64_t hostmem_dma_v2p(void *virt);
 *    Resolve a virtual address to its corresponding physical address.
 *
 *  - void hostmem_dma_term(void);
 *    Release all memory and internal structures associated with the allocator.
 *
 * Usage
 * -----
 *
 * You must call `hostmem_dma_init()` before any allocation is made, and `hostmem_dma_term()` after
 * all memory has been freed. The allocator does not support lazy initialization.
 *
 * Caveats
 * -------
 *
 * - Physical contiguity is guaranteed only up to the system's hugepage size. On most systems, this
 *   is 2MB.
 *
 * - Sub-hugepage allocations may span multiple hugepages, resulting in reduced physical
 *   contiguity. This may be addressed in a future update.
 *
 * - Alignment is currently to the system's page size (typically 4KB).
 *
 * Roadmap
 * -------
 *
 * Planned improvements include:
 *
 *   - hostmem_dma_calloc() for zero-initialized memory
 *   - hostmem_dma_aligned_alloc() for custom alignment
 *   - Sub-hugepage contiguity enforcement
 *
 * @file hostmem_dma.h
 */

struct hostmem_heap g_hostmem_dma = {0};

/**
 * Release all memory and internal metadata associated with the DMA allocator.
 *
 * This must be called after all allocations have been freed via hostmem_dma_free().
 */
static inline void
hostmem_dma_term(void)
{
	hostmem_heap_term(&g_hostmem_dma);
}

/**
 * Initialize the DMA allocator and reserve hugepage-backed memory.
 *
 * @param size Number of bytes to reserve from hugepage memory.
 * @return On success, 0 is returned. On error, negative errno is returned to indicate the error.
 */
static inline int
hostmem_dma_init(size_t size)
{
	int err;

	err = hostmem_state_init(&g_hostmem_state);
	if (err) {
		return err;
	}

	return hostmem_heap_init(&g_hostmem_dma, size);
}

/**
 * Free the DMA-capable memory pointed to by `ptr`
 *
 * If `ptr` is NULL, no operation is performed.
 *
 * @param ptr Pointer previously returned by hostmem_dma_malloc().
 */
static inline void
hostmem_dma_free(void *ptr)
{
	hostmem_heap_block_free(&g_hostmem_dma, ptr);
}

/**
 * Allocate `size` bytes of DMA-capable memory
 *
 * @param size Number of bytes to allocate.
 * @return Pointer to the allocated memory, or NULL on failure.
 */
static inline void *
hostmem_dma_malloc(size_t size)
{
	return hostmem_heap_block_alloc(&g_hostmem_dma, size);
}

/**
 * Resolve the physical address of a given virtual address.
 *
 * @param virt Pointer to memory previously allocated by hostmem_dma_malloc().
 * @return Physical address corresponding to the given virtual address.
 */
static inline uint64_t
hostmem_dma_v2p(void *virt)
{
	size_t offset, hpage_idx, in_hpage_offset;

	// Compute byte offset from base of heap
	offset = (char *)virt - (char *)g_hostmem_dma.memory.virt;

	// Determine which hugepage this address falls into
	hpage_idx = offset / g_hostmem_state.hugepgsz;

	// Offset within that hugepage
	in_hpage_offset = offset % g_hostmem_state.hugepgsz;

	return g_hostmem_dma.phys_lut[hpage_idx] + in_hpage_offset;
}
