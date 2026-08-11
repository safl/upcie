// SPDX-License-Identifier: BSD-3-Clause

/**
 * Experimental iommu-map-pa helper interface
 * ==========================================
 *
 * ==========================================================================
 * EXPERIMENTAL DEPENDENCY
 * Requires the out-of-tree iommu-map-pa DKMS module and /dev/iommu_map_pa.
 * Unlike <upcie/experimental/dmabuf_import.h> this header has no stub path:
 * without the ABI, from the installed package or the in-tree fallback below,
 * including it is a compile error rather than a runtime -ENOTSUP.
 * ==========================================================================
 *
 * Userspace wrappers for the helper kernel module that maps an array of
 * physical addresses (e.g. a CUDA/dma-buf-derived phys_lut) into the IOMMU
 * domain a VFIO-controlled device already uses, returning an IOVA base that
 * userspace addresses the memory through. For an NVMe that means writing the
 * IOVA into PRPs.
 *
 * Mappings persist until userspace issues an explicit UNMAP or closes the file
 * descriptor.
 *
 * The ioctl ABI comes from <linux/iommu_map_pa.h>, installed system-wide by the
 * iommu-map-pa DKMS package. Builds from this checkout fall back to the in-tree
 * module header so the package is not required for development.
 *
 * @file iommu_map_pa.h
 * @version 0.5.2
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
#define UPCIE_IOMMU_MAP_PA_UAPI_SYSTEM 1
#endif
#endif
#ifndef UPCIE_IOMMU_MAP_PA_UAPI_SYSTEM
#include "../../../module/iommu_map_pa.h"
#endif

/* Reaching this line means the ABI resolved, so consumers that include this
 * header conditionally can test the macro. It is never 0: the include above
 * fails first when the ABI is missing. */
#define UPCIE_HAVE_IOMMU_MAP_PA 1

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
 * Map an array of physical addresses (phys_lut) into the IOMMU domain the
 * target device currently uses. On success the IOMMU translates
 * 'iova_base + i * page_size' to 'phys[i]', so PRPs should be built from
 * iova_base, not from phys[].
 *
 * 'device_fd' is the open VFIO device fd for 'bdf', and is required: the module
 * pins it for the lifetime of the mapping, which keeps the device attached and
 * so keeps the domain from being torn down by the ordinary teardown paths. It
 * is not a guarantee, an explicit detach still works, so unmap before tearing
 * the VFIO setup down.
 */
static inline int
upcie_iommu_map_pa_add(int fd, const char *bdf, int dmabuf_fd, int device_fd,
				uint64_t iova_base, __u32 page_size, __u32 nphys,
				const uint64_t *phys, __u32 prot,
				uint64_t *map_handle_out)
{
	struct iommu_map_pa_req req = {0};

	if (fd < 0 || !bdf || !phys || !nphys)
		return -EINVAL;
	/* Required by the module: it pins this fd so the ordinary VFIO teardown
	 * paths cannot detach the device while the mapping is installed. */
	if (device_fd <= 0)
		return -EINVAL;

	strncpy(req.bdf, bdf, sizeof(req.bdf) - 1);
	/* Normalise "no dma-buf": fd 0 is a valid descriptor, so a plain
	 * negative value is the only safe way to say "nothing to pin". */
	req.dmabuf_fd = dmabuf_fd < 0 ? IOMMU_MAP_PA_NO_DMABUF : dmabuf_fd;
	req.device_fd = device_fd;
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
