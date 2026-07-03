// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * DMA memory abstraction
 * ======================
 *
 * A dmamem describes memory usable as a DMA source or destination,
 * regardless of which exporter produced it (memfd hugepage, CUDA VRAM,
 * HIP VRAM, libdrm BO, VFIO BAR) and regardless of which kernel API
 * installed the DMA mapping. Cousin of the kernel's dma-buf, at the
 * layer above.
 *
 * A dmamem holds an fd, an optional CPU virtual address (NULL for peer
 * memory that is not CPU-mappable), a size, and a (base_iova, size)
 * tuple recorded once at construction time. At submission the DMA
 * address is:
 *
 *     iova = base_iova + offset
 *
 * where offset is the caller-chosen position inside the range. No
 * pagemap walk, no per-buffer table, no branch. The dmamem does not own
 * the mapping context; it belongs to the enclosing controller and is
 * passed in.
 *
 * Mapping context in this implementation. The current constructors bind
 * the map/unmap side to iommufd: (base_iova, size) comes from
 * IOMMU_IOAS_MAP_FILE against an iommufd IOAS, and dmamem_destroy
 * unmaps via IOMMU_IOAS_UNMAP. Legacy VFIO type1 container and no-IOMMU
 * (passthrough) variants would fit the same public shape with different
 * mapper calls and are not built today; iommufd is where new kernel
 * features (dma-buf import, dirty tracking, PASID) land and is the
 * target for the rest of the design.
 *
 * Constructors:
 *
 *   dmamem_from_memfd(...) in dmamem_memfd.h creates a hugepage-backed
 *   memfd internally and imports it into an IOAS.
 *
 * dmamem_destroy() unmaps and releases in every case.
 *
 * @file dmamem.h
 * @version 0.5.1
 */

enum dmamem_backing {
	DMAMEM_BACKING_UNKNOWN = 0x0,
	DMAMEM_BACKING_MEMFD = 0x1,
	DMAMEM_BACKING_DMABUF = 0x2,
};

/**
 * A DMA-capable memory region mapped into an iommufd IOAS
 */
struct dmamem {
	int fd;                     ///< memfd or dma-buf; owned by dmamem
	void *cpu_va;               ///< CPU virtual address, NULL when not mappable
	size_t size;                ///< Size in bytes
	uint64_t base_iova;         ///< Base IOVA in the IOAS
	struct iommufd *iommufd;    ///< Not owned; caller owns lifetime; carries the IOAS id
	enum dmamem_backing backing;
};

/**
 * Print information about the given dmamem
 */
static inline int
dmamem_pp(struct dmamem *dmem)
{
	int wrtn = 0;

	wrtn += printf("dmamem:");

	if (!dmem) {
		wrtn += printf(" ~\n");
		return 0;
	}

	wrtn += printf("\n");
	wrtn += printf("  fd: %d\n", dmem->fd);
	wrtn += printf("  cpu_va: %p\n", dmem->cpu_va);
	wrtn += printf("  size: %zu\n", dmem->size);
	wrtn += printf("  base_iova: 0x%" PRIx64 "\n", dmem->base_iova);
	wrtn += printf("  ioas_id: %u\n", dmem->iommufd ? dmem->iommufd->ioas_id : 0);
	wrtn += printf("  iommufd.fd: %d\n", dmem->iommufd ? dmem->iommufd->fd : -1);
	wrtn += printf("  backing: %d\n", dmem->backing);

	return wrtn;
}

/**
 * Convert an offset inside the dmamem to an IOVA.
 *
 * The submission-path fast function; one addition, no lookup, no branch.
 */
static inline uint64_t
dmamem_offset_to_iova(struct dmamem *dmem, size_t offset)
{
	return dmem->base_iova + offset;
}

/**
 * Convert a CPU VA inside the dmamem to an IOVA.
 *
 * Only usable when the backing exposes a CPU VA. Callers must assert
 * dmem->cpu_va != NULL before invoking; the fast path does not check.
 */
static inline uint64_t
dmamem_va_to_iova(struct dmamem *dmem, void *vaddr)
{
	assert(dmem->cpu_va);
	return dmem->base_iova + ((char *)vaddr - (char *)dmem->cpu_va);
}

/**
 * Unmap the dmamem from the IOAS, munmap the CPU view (if any), and
 * close the underlying fd.
 *
 * The caller-owned iommufd handle stays open.
 */
static inline void
dmamem_destroy(struct dmamem *dmem)
{
	if (!dmem) {
		return;
	}

	if (dmem->iommufd && dmem->size) {
		int err = iommufd_ioas_unmap(dmem->iommufd, dmem->base_iova, dmem->size);
		if (err) {
			UPCIE_DEBUG("FAILED: iommufd_ioas_unmap(); err(%d)", err);
		}
	}

	if (dmem->cpu_va && dmem->size) {
		munmap(dmem->cpu_va, dmem->size);
	}

	if (dmem->fd >= 0) {
		close(dmem->fd);
	}

	memset(dmem, 0, sizeof(*dmem));
	dmem->fd = -1;
}
