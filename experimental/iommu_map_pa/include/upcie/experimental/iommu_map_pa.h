// SPDX-License-Identifier: BSD-3-Clause

/**
 * Experimental uPCIe iommu-map helper interface
 * ==============================================
 *
 * Userspace wrappers for the helper kernel module that maps an array of
 * device-physical addresses (e.g. a CUDA/udmabuf-derived phys_lut) into the
 * IOMMU domain a VFIO-controlled NVMe already uses, returning an IOVA base that
 * userspace writes into NVMe PRPs.
 *
 * Mappings persist until userspace issues an explicit UNMAP or closes the file
 * descriptor.
 *
 * The ioctl ABI comes from <linux/iommu_map_pa.h>, installed system-wide by
 * the iommu-map-pa DKMS package. Builds from this checkout fall back to the
 * module header next to it, so the package is not required for development.
 *
 * @file iommu_map_pa.h
 * @version 0.5.1
 */
#ifndef UPCIE_EXPERIMENTAL_IOMMU_MAP_PA_H
#define UPCIE_EXPERIMENTAL_IOMMU_MAP_PA_H

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#if defined(__has_include)
#if __has_include(<linux/iommu_map_pa.h>)
#include <linux/iommu_map_pa.h>
#define IOMMU_MAP_PA_UAPI_SYSTEM 1
#endif
#endif
#ifndef IOMMU_MAP_PA_UAPI_SYSTEM
#include "../../../module/iommu_map_pa.h"
#endif

static inline int
upcie_iommu_map_pa_open(void)
{
	int fd = open(IOMMU_MAP_PA_DEVPATH, O_RDWR);

	return fd < 0 ? -errno : fd;
}

static inline int
upcie_iommu_map_pa_close(int fd)
{
	if (fd < 0)
		return -EINVAL;

	return close(fd) ? -errno : 0;
}

/* Unmap a handle returned by upcie_iommu_map_pa_add(). */
static inline int
upcie_iommu_map_pa_del(int fd, __u64 map_handle)
{
	struct iommu_unmap_pa_req req = {0};

	if (fd < 0 || !map_handle)
		return -EINVAL;

	req.map_handle = map_handle;
	return ioctl(fd, IOMMU_UNMAP_PA, &req) < 0 ? -errno : 0;
}

/*
 * Map an array of device-physical addresses (phys_lut) into the
 * IOMMU domain the target NVMe device currently uses. On success the IOMMU
 * translates 'iova_base + i * page_size' to 'phys[i]', so PRPs should be built
 * from iova_base, not from phys[].
 */
static inline int
upcie_iommu_map_pa_add(int fd, const char *bdf, int dmabuf_fd,
				uint64_t iova_base, __u32 page_size, __u32 nphys,
				const uint64_t *phys, __u32 prot,
				uint64_t *map_handle_out)
{
	struct iommu_map_pa_req req = {0};

	if (fd < 0 || !bdf || !phys || !nphys)
		return -EINVAL;

	strncpy(req.bdf, bdf, sizeof(req.bdf) - 1);
	req.dmabuf_fd = dmabuf_fd;
	req.page_size = page_size;
	req.nphys = nphys;
	req.prot = prot;
	req.iova_base = iova_base;
	req.user_phys_ptr = (__u64)(uintptr_t)phys;

	if (ioctl(fd, IOMMU_MAP_PA, &req) < 0)
		return -errno;

	if (map_handle_out)
		*map_handle_out = req.map_handle;
	return 0;
}

#endif
