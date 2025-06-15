// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
  Malloc-like memory allocator backed by hugepages for DMA in user-space drivers
  ==============================================================================

  * hostmem_dma_malloc()
  * hostmem_dma_free()
  * hostmem_dma_v2p()

  Behavior
  --------

  By default 1GB of hugepages are reserved. To control this, then invoke hostmem_dma_init() before
  any calls to hostmem_dma_alloc().

  CAVEAT
  ------

  The allocator will provide you with an allocation which is contig. in VA-space. However, the
  same contig. property can only be guaranteed up to the size of a hugepage on the system. In case
  you do no explicit setup then the default for hugepages are 2MB. Thus, you are only guaranteed
  physical contig. up to 2MB. Be aware of this.

  Also, currently, then the allocations provided by the head does not check for hugepage
  boundaries, thus, you will most likely have an allocation with even less guarantee contig.
  PA-space. This latter part should be fixed. E.g. when one allocates less than hugepage-size, then
  it should not split it over a hugepage boundary.

  Future
  ------

  This is still not a complete malloc-replacement, for that the following must be implemented.

  * Add calloc
  * Add alligned-alloc -- current alignment is to host pagesize

  @file hostmem_dma.h
*/
#ifndef UPCIE_HOSTMEM_DMA_H
#define UPCIE_HOSTMEM_DMA_H

#define _GNU_SOURCE
#include <fcntl.h>
#include <hostmem.h>
#include <hostmem_heap.h>
#include <inttypes.h>
#include <linux/memfd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#define HOSTMEM_DMA_DEFAULT_REGION_SIZE 1024 * 1024 * 1024ULL

struct hostmem_heap g_hostmem_dma = {0};

static inline int
hostmem_dma_init(size_t size)
{
	return hostmem_heap_init(&g_hostmem_dma, size);
}

static inline void
hostmem_dma_free(void *ptr)
{
	hostmem_heap_block_free(&g_hostmem_dma, ptr);
}

static inline void *
hostmem_dma_malloc(size_t size)
{
	if (!g_hostmem_dma.nphys) {
		hostmem_dma_init(HOSTMEM_DMA_DEFAULT_REGION_SIZE);
	}

	return hostmem_heap_block_alloc(&g_hostmem_dma, size);
}

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

#endif // UPCIE_HOSTMEM_DMA_H