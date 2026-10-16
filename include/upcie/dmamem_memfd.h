// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * dmamem constructor: memfd
 * =========================
 *
 * Creates a memfd, backed by hugepages (memfd_create(MFD_HUGETLB | ...)) or by
 * plain 4 KiB pages, mmaps it for CPU access, and imports the range into an
 * iommufd IOAS with a single IOMMU_IOAS_MAP_FILE call. The result is a dmamem
 * whose cpu_va is the mmap base and whose base_iova is the IOVA the kernel
 * picked, or the one the caller fixed with dmamem_from_memfd_at().
 *
 * Caveat: for hugepages the pool must be reserved out-of-band (see the
 * `hugepages` CLI or `echo N > /proc/sys/vm/nr_hugepages`). Either way the
 * process must have IPC_LOCK / adequate memlock limits.
 *
 * @file dmamem_memfd.h
 * @version 0.10.4
 */

#ifndef MFD_HUGE_2MB
#define MFD_HUGE_2MB (21 << 26)
#endif
#ifndef MFD_HUGE_1GB
#define MFD_HUGE_1GB (30 << 26)
#endif

/**
 * Allocate a memfd and map it into the handle's IOAS, at an IOVA of the
 * caller's choosing or of the kernel's.
 *
 * @param dmem     Pre-allocated dmamem descriptor to fill.
 * @param iommufd  Open iommufd handle with an allocated IOAS (caller-owned).
 * @param size     Size in bytes; must be a multiple of pgsz.
 * @param pgsz     Page size in bytes: 4 KiB for a plain memfd, 2 MiB or 1 GiB
 *                 for hugepages.
 * @param flags    Bitmask of IOMMU_IOAS_MAP_* flags. With
 *                 IOMMU_IOAS_MAP_FIXED_IOVA the range is mapped at `iova`;
 *                 without it the kernel picks.
 * @param iova     Where to map under IOMMU_IOAS_MAP_FIXED_IOVA; ignored
 *                 otherwise.
 *
 * @return 0 on success, negative errno on error.
 */
static inline int
dmamem_from_memfd_at(struct dmamem *dmem, struct iommufd *iommufd, size_t size, size_t pgsz,
		     uint32_t flags, uint64_t iova)
{
	unsigned int memfd_flags;
	int err;

	if (!dmem || !iommufd || iommufd->fd < 0 || !size) {
		return -EINVAL;
	}

	if (pgsz == 4096) {
		memfd_flags = 0;
	} else if (pgsz == 2ULL * 1024 * 1024) {
		memfd_flags = MFD_HUGETLB | MFD_HUGE_2MB;
	} else if (pgsz == 1024ULL * 1024 * 1024) {
		memfd_flags = MFD_HUGETLB | MFD_HUGE_1GB;
	} else {
		UPCIE_DEBUG("FAILED: unsupported pgsz(%zu)", pgsz);
		return -EINVAL;
	}

	if (size % pgsz) {
		UPCIE_DEBUG("FAILED: size(%zu) not a multiple of pgsz(%zu)", size, pgsz);
		return -EINVAL;
	}

	memset(dmem, 0, sizeof(*dmem));
	dmem->fd = -1;
	dmem->iommufd = iommufd;
	dmem->size = size;
	dmem->backing = DMAMEM_BACKING_MEMFD;
	dmem->translator = DMAMEM_XLATE_ARITHMETIC;
	dmem->owned = 1;

	dmem->fd = syscall(SYS_memfd_create, "dmamem", memfd_flags);
	if (dmem->fd < 0) {
		err = -errno;
		UPCIE_DEBUG("FAILED: memfd_create(); errno(%d)", errno);
		return err;
	}

	if (ftruncate(dmem->fd, size) != 0) {
		err = -errno;
		UPCIE_DEBUG("FAILED: ftruncate(); errno(%d)", errno);
		goto err_close;
	}

	dmem->cpu_va = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, dmem->fd, 0);
	if (dmem->cpu_va == MAP_FAILED) {
		err = -errno;
		dmem->cpu_va = NULL;
		UPCIE_DEBUG("FAILED: mmap(); errno(%d)", errno);
		goto err_close;
	}

	if (mlock(dmem->cpu_va, size) != 0) {
		err = -errno;
		UPCIE_DEBUG("FAILED: mlock(); errno(%d)", errno);
		goto err_munmap;
	}

	/*
	 * Zero the range to fault in and pin every backing page before the
	 * IOAS map. Otherwise IOMMU_IOAS_MAP_FILE may pin a partial or
	 * still-being-materialised range.
	 */
	memset(dmem->cpu_va, 0, size);

	dmem->base_iova = iova;
	err = iommufd_ioas_map_file(iommufd, dmem->fd, 0, size, flags, &dmem->base_iova);
	if (err) {
		UPCIE_DEBUG("FAILED: iommufd_ioas_map_file(); err(%d)", err);
		goto err_munmap;
	}
	dmem->base_va = dmem->cpu_va;

	return 0;

err_munmap:
	munmap(dmem->cpu_va, size);
	dmem->cpu_va = NULL;
err_close:
	close(dmem->fd);
	memset(dmem, 0, sizeof(*dmem));
	dmem->fd = -1;
	return err;
}

/**
 * Allocate a memfd and map it into the handle's IOAS where the kernel picks.
 *
 * @param dmem     Pre-allocated dmamem descriptor to fill.
 * @param iommufd  Open iommufd handle with an allocated IOAS (caller-owned).
 * @param size     Size in bytes; must be a multiple of pgsz.
 * @param pgsz     Page size in bytes: 4 KiB for a plain memfd, 2 MiB or 1 GiB
 *                 for hugepages.
 *
 * @return 0 on success, negative errno on error.
 */
static inline int
dmamem_from_memfd(struct dmamem *dmem, struct iommufd *iommufd, size_t size, size_t pgsz)
{
	return dmamem_from_memfd_at(dmem, iommufd, size, pgsz,
				    IOMMU_IOAS_MAP_READABLE | IOMMU_IOAS_MAP_WRITEABLE, 0);
}
