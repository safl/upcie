// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * dmamem constructors: caller memory by address, and IOVAs mapped elsewhere
 * ==========================================================================
 *
 * Two wrapping constructors for memory whose backing the caller manages.
 *
 * dmamem_from_vaddr() maps a range of the caller's address space, any memory
 * the kernel can pin (anonymous, a file mapping, a heap allocation), into an
 * iommufd IOAS with IOMMU_IOAS_MAP. The caller keeps the memory; destroying
 * the dmamem only removes the mapping, and the memory must stay mapped in the
 * process until it has.
 *
 * dmamem_adopt_iova() describes a range of an IOAS that someone else mapped,
 * such as another process sharing the iommufd that mapped its own memory and
 * passed on the IOVA. The adopting side has no CPU view of it, so base_va and
 * cpu_va stay NULL and only dmamem_offset_to_iova() translates. Destroying the
 * dmamem unmaps the range from the IOAS, which takes the device's access to it
 * away; the memory itself stays with whoever mapped it.
 *
 * @file dmamem_vaddr.h
 * @version 0.9.0
 */

/**
 * Map caller memory into the handle's IOAS as a dmamem.
 *
 * @param dmem     Pre-allocated dmamem descriptor to fill.
 * @param iommufd  Open iommufd handle with an allocated IOAS (caller-owned).
 * @param vaddr    Start of the memory; the kernel requires a fixed IOVA to
 *                 share its offset within a page.
 * @param size     Size in bytes.
 * @param flags    Bitmask of IOMMU_IOAS_MAP_* flags. With
 *                 IOMMU_IOAS_MAP_FIXED_IOVA the range is mapped at `iova`;
 *                 without it the kernel picks.
 * @param iova     Where to map under IOMMU_IOAS_MAP_FIXED_IOVA; ignored
 *                 otherwise.
 *
 * @return 0 on success, negative errno on error.
 */
static inline int
dmamem_from_vaddr(struct dmamem *dmem, struct iommufd *iommufd, void *vaddr, size_t size,
		  uint32_t flags, uint64_t iova)
{
	int err;

	if (!dmem || !iommufd || iommufd->fd < 0 || !vaddr || !size) {
		return -EINVAL;
	}

	memset(dmem, 0, sizeof(*dmem));
	dmem->fd = -1;

	err = iommufd_ioas_map(iommufd, (uint64_t)(uintptr_t)vaddr, size, flags, &iova);
	if (err) {
		UPCIE_DEBUG("FAILED: iommufd_ioas_map(); err(%d)", err);
		return err;
	}

	dmem->base_va = vaddr;
	dmem->cpu_va = vaddr;
	dmem->size = size;
	dmem->base_iova = iova;
	dmem->iommufd = iommufd;
	dmem->backing = DMAMEM_BACKING_VADDR;
	dmem->translator = DMAMEM_XLATE_ARITHMETIC;
	dmem->owned = 0;

	return 0;
}

/**
 * Describe a range someone else mapped into the handle's IOAS as a dmamem.
 *
 * Nothing is mapped and nothing is checked: the range is taken to be mapped
 * at `iova` for `size` bytes. dmamem_destroy() unmaps it.
 *
 * @param dmem     Pre-allocated dmamem descriptor to fill.
 * @param iommufd  Open iommufd handle whose IOAS holds the range
 *                 (caller-owned).
 * @param iova     Start of the range in the IOAS.
 * @param size     Size in bytes.
 *
 * @return 0 on success, negative errno on error.
 */
static inline int
dmamem_adopt_iova(struct dmamem *dmem, struct iommufd *iommufd, uint64_t iova, size_t size)
{
	if (!dmem || !iommufd || iommufd->fd < 0 || !size) {
		return -EINVAL;
	}

	memset(dmem, 0, sizeof(*dmem));
	dmem->fd = -1;
	dmem->size = size;
	dmem->base_iova = iova;
	dmem->iommufd = iommufd;
	dmem->backing = DMAMEM_BACKING_IOVA;
	dmem->translator = DMAMEM_XLATE_ARITHMETIC;
	dmem->owned = 0;

	return 0;
}
