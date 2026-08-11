/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_UPCIE_IOMMU_MAP_H
#define _UAPI_LINUX_UPCIE_IOMMU_MAP_H

#include <linux/ioctl.h>
#include <linux/types.h>

/*
 * UAPI for the experimental upcie iommu-map helper (/dev/upcie-iommu-map).
 *
 * The module maps an array of device-physical addresses (e.g. a
 * CUDA/udmabuf-derived phys_lut) into the IOMMU domain a VFIO-controlled
 * NVMe already uses, returning an IOVA base that userspace writes into
 * NVMe PRPs. Mappings persist until userspace issues an explicit UNMAP or
 * closes the file descriptor.
 *
 * Installed system-wide as <upcie/upcie_iommu_map.h> by the
 * upcie-iommu-map-dkms package.
 */

/*
 * Device node the helper module registers (miscdevice "upcie-iommu-map").
 * Open this for the map ioctls; override with -DUPCIE_IOMMU_MAP_DEVICE=...
 */
#ifndef UPCIE_IOMMU_MAP_DEVICE
#define UPCIE_IOMMU_MAP_DEVICE "/dev/upcie-iommu-map"
#endif

#define UPCIE_IOMMU_MAP_BDF_LEN 16

/* Protection bits for UPCIE_IOMMU_MAP (0 is treated as READ|WRITE). */
#define UPCIE_IOMMU_MAP_PROT_READ (1U << 0)
#define UPCIE_IOMMU_MAP_PROT_WRITE (1U << 1)

/*
 * Value for 'dmabuf_fd' when no dma-buf should be pinned. Any negative value
 * works; 0 does not, since it is a valid descriptor. Always set this field
 * explicitly: a '{0}'-initialised request otherwise names fd 0 (stdin).
 */
#define UPCIE_IOMMU_MAP_NO_DMABUF (-1)

/* Upper bound on 'nphys', to keep a bogus request from asking for a
 * multi-gigabyte kernel allocation. 4 MiB of entries, 2 GiB at 4K pages. */
#define UPCIE_IOMMU_MAP_MAX_NPHYS (1U << 19)

struct upcie_iommu_unmap_req {
	__aligned_u64 map_handle;
};

/*
 * Map an already-known array of device-physical addresses
 * (e.g. a udmabuf-derived phys_lut) directly into the IOMMU domain that the
 * target NVMe device currently uses. This is meant for a VFIO-controlled
 * device: userspace picks 'iova_base' (must not collide with its own
 * VFIO_IOMMU_MAP_DMA mappings) and programs PRPs with iova_base + offset.
 */
struct upcie_iommu_map_req {
	char bdf[UPCIE_IOMMU_MAP_BDF_LEN];
	__s32 dmabuf_fd;		/* optional lifetime ref;
					 * UPCIE_IOMMU_MAP_NO_DMABUF to skip */
	__u32 page_size;
	__u32 nphys;			/* <= UPCIE_IOMMU_MAP_MAX_NPHYS */
	__u32 prot;			/* UPCIE_IOMMU_MAP_PROT_*; 0 => READ|WRITE */
	/* Fills the padding before the 8-byte aligned members and reserves it
	 * for future use; must be zero, the module rejects anything else. */
	__u32 reserved[2];
	__aligned_u64 iova_base;	/* userspace-chosen base IOVA */
	__aligned_u64 user_phys_ptr;	/* array of 'nphys' __u64 phys addrs */
	__aligned_u64 map_handle;	/* out */
};

#define UPCIE_IOMMU_MAP_IOC_MAGIC 'u'

#define UPCIE_IOMMU_UNMAP _IOW(UPCIE_IOMMU_MAP_IOC_MAGIC, 0x02, \
				struct upcie_iommu_unmap_req)
#define UPCIE_IOMMU_MAP _IOWR(UPCIE_IOMMU_MAP_IOC_MAGIC, 0x03, \
				     struct upcie_iommu_map_req)

#endif
