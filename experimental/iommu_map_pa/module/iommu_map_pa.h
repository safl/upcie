/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_IOMMU_MAP_PA_H
#define _UAPI_LINUX_IOMMU_MAP_PA_H

#include <linux/ioctl.h>
#include <linux/types.h>

/*
 * UAPI for the experimental iommu-map-pa helper (/dev/iommu_map_pa).
 *
 * The module maps an array of physical addresses (e.g. a dma-buf-derived
 * phys_lut for GPU memory) into the IOMMU domain a device already uses,
 * returning an IOVA base that translates back to them. The target must sit in
 * a userspace-owned (unmanaged) domain, i.e. be bound to vfio-pci or iommufd;
 * a driver-bound device is refused. For a VFIO-controlled NVMe the returned
 * IOVA can be written straight into PRPs. Mappings persist until userspace
 * issues an explicit UNMAP or closes the file descriptor.
 *
 * Installed system-wide as <linux/iommu_map_pa.h> by the iommu-map-pa-dkms
 * package.
 */

/*
 * Device node the helper module registers (miscdevice "iommu_map_pa").
 * Open this for the map ioctls; override with -DIOMMU_MAP_PA_DEVPATH=...
 */
#ifndef IOMMU_MAP_PA_DEVPATH
#define IOMMU_MAP_PA_DEVPATH "/dev/iommu_map_pa"
#endif

#define IOMMU_MAP_PA_BDF_LEN 16

/* Protection bits for IOMMU_MAP_PA (0 is treated as READ|WRITE). Any other bit
 * is rejected with -EINVAL, so the rest of the field stays free for future
 * flags. */
#define IOMMU_MAP_PA_PROT_READ (1U << 0)
#define IOMMU_MAP_PA_PROT_WRITE (1U << 1)

/*
 * Value for 'dmabuf_fd' when no dma-buf should be pinned. Any negative value
 * works; 0 does not, since it is a valid descriptor. Always set this field
 * explicitly: a '{0}'-initialised request otherwise names fd 0 (stdin).
 */
#define IOMMU_MAP_PA_NO_DMABUF (-1)

/* Upper bound on 'nphys', to keep a bogus request from asking for a
 * multi-gigabyte kernel allocation. 4 MiB of entries, 2 GiB at 4K pages. */
#define IOMMU_MAP_PA_MAX_NPHYS (1U << 19)

struct iommu_unmap_pa_req {
	__aligned_u64 map_handle;
};

/*
 * Map an already-known array of physical addresses (e.g. a dma-buf-derived
 * phys_lut) directly into the IOMMU domain that the target device currently
 * uses. Userspace picks 'iova_base', which must not collide with its own
 * VFIO_IOMMU_MAP_DMA mappings, and addresses the memory as iova_base + offset;
 * for an NVMe that means writing that into PRPs.
 */
struct iommu_map_pa_req {
	char bdf[IOMMU_MAP_PA_BDF_LEN];
	__s32 dmabuf_fd;		/* optional lifetime ref;
					 * IOMMU_MAP_PA_NO_DMABUF to skip */
	__u32 page_size;
	__u32 nphys;			/* <= IOMMU_MAP_PA_MAX_NPHYS */
	__u32 prot;			/* IOMMU_MAP_PA_PROT_*; 0 => READ|WRITE */
	/* Fills the padding before the 8-byte aligned members and reserves it
	 * for future use; must be zero, the module rejects anything else. */
	__u32 reserved[2];
	__aligned_u64 iova_base;	/* userspace-chosen base IOVA */
	__aligned_u64 user_phys_ptr;	/* array of 'nphys' __u64 phys addrs */
	__aligned_u64 map_handle;	/* out */
};

#define IOMMU_MAP_PA_IOC_MAGIC 'u'

#define IOMMU_UNMAP_PA _IOW(IOMMU_MAP_PA_IOC_MAGIC, 0x02, \
				struct iommu_unmap_pa_req)
#define IOMMU_MAP_PA _IOWR(IOMMU_MAP_PA_IOC_MAGIC, 0x03, \
				     struct iommu_map_pa_req)

#endif
