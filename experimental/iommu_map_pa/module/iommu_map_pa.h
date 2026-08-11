/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_IOMMU_MAP_PA_H
#define _UAPI_LINUX_IOMMU_MAP_PA_H

#include <linux/ioctl.h>
#include <linux/types.h>

/*
 * UAPI for the experimental iommu-map-pa helper (/dev/iommu_map_pa).
 *
 * The module maps an array of device-physical addresses (e.g. a
 * CUDA/udmabuf-derived phys_lut) into the IOMMU domain a VFIO-controlled
 * NVMe already uses, returning an IOVA base that userspace writes into
 * NVMe PRPs. Mappings persist until userspace issues an explicit UNMAP or
 * closes the file descriptor.
 *
 * Installed system-wide as <linux/iommu_map_pa.h> by the
 * iommu-map-pa-dkms package.
 */

/* Override with -DIOMMU_MAP_PA_DEVPATH=... */
#ifndef IOMMU_MAP_PA_DEVPATH
#define IOMMU_MAP_PA_DEVPATH "/dev/iommu_map_pa"
#endif

#define IOMMU_MAP_PA_BDF_LEN 16

/* Protection bits for IOMMU_MAP_PA (0 is treated as READ|WRITE). */
#define IOMMU_MAP_PA_PROT_READ (1U << 0)
#define IOMMU_MAP_PA_PROT_WRITE (1U << 1)

struct iommu_unmap_pa_req {
	__aligned_u64 map_handle;
};

/*
 * Map an already-known array of device-physical addresses
 * (e.g. a udmabuf-derived phys_lut) directly into the IOMMU domain that the
 * target NVMe device currently uses. This is meant for a VFIO-controlled
 * device: userspace picks 'iova_base' (must not collide with its own
 * VFIO_IOMMU_MAP_DMA mappings) and programs PRPs with iova_base + offset.
 */
struct iommu_map_pa_req {
	char bdf[IOMMU_MAP_PA_BDF_LEN];
	__s32 dmabuf_fd;		/* optional lifetime ref; <0 to skip */
	__u32 page_size;
	__u32 nphys;
	__u32 prot;			/* IOMMU_MAP_PA_PROT_*; 0 => READ|WRITE */
	__u32 reserved;
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
