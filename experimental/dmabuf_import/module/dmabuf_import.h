/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_DMABUF_IMPORT_H
#define _UAPI_LINUX_DMABUF_IMPORT_H

#include <linux/types.h>
#include <linux/ioctl.h>

/*
 * UAPI for the out-of-tree dma-buf importer (/dev/dmabuf_import), installed
 * system-wide by the dmabuf-import-dkms package.
 *
 * The magic ('u') and numbers come from the udmabuf patch these ioctls were
 * first delivered as, which is the only reason they look the way they do.
 * They are issued on a separate fd, so they never clash with the stock
 * /dev/udmabuf ioctls.
 *
 * Attachments are owned by the descriptor that created them: GET_MAP and
 * DETACH must be issued on the same one as the ATTACH, and closing it tears
 * the attachment down. Keep it open while the DMA addresses are in use.
 */

/* Override with -DDMABUF_IMPORT_DEVPATH=... */
#ifndef DMABUF_IMPORT_DEVPATH
#define DMABUF_IMPORT_DEVPATH "/dev/dmabuf_import"
#endif

/**
 * struct dmabuf_import_attach - import dma-buf and return number of addresses
 */
struct dmabuf_import_attach {
	/** @fd: dma-buf file descriptor (in) */
	__s32 fd;
	/** @count: Count of DMA addresses in the dma-buf (out) */
	__u32 count;
};

/**
 * struct dmabuf_import_dma_map - Representation of a DMA mapping
 */
struct dmabuf_import_dma_map {
	/** @dma_addr: DMA address of the mapping */
	__u64 dma_addr;
	/** @dma_len: Length of the mapping */
	__u64 dma_len;
};

/**
 * struct dmabuf_import_get_map - Get DMA mappings from the provided dma-buf fd
 *
 * The dma-buf fd must match one previously passed to DMABUF_IMPORT_ATTACH on
 * this same descriptor. The dma_arr array is allocated by userspace.
 */
struct dmabuf_import_get_map {
	/** @fd: dma-buf file descriptor (in) */
	__s32 fd;
	/** @count: Size of the dma_arr array (in) */
	__u32 count;
	/** @dma_arr: Array of DMA mappings (out) */
	struct dmabuf_import_dma_map dma_arr[];
};

/** Room for "0000:00:00.0", matching what sysfs prints. */
#define DMABUF_IMPORT_BDF_LEN 16

/**
 * struct dmabuf_import_attach_bdf - Import a dma-buf for a specific PCI device
 *
 * The device named here is the one that will read the memory, and it is the
 * device the exporter is asked about. That matters: asked on behalf of this
 * module's own misc device, which has no PCI parent, an exporter has no bus
 * path to weigh and may migrate the buffer to system memory rather than hand
 * out its own. Named a PCI device, it can answer the question that was
 * actually meant, and the mapping lands in that device's IOMMU domain, which
 * is where a caller programming these addresses into it needs them.
 *
 * Named for what it takes: a PCI address is the only thing that can be given,
 * since peer-to-peer is a question about a path between PCI devices. Naming a
 * device some other way would be a different ioctl, not a wider field here.
 *
 * DMABUF_IMPORT_ATTACH remains the misc-device form, for callers that only
 * want the addresses and do not care which device reaches them.
 */
struct dmabuf_import_attach_bdf {
	/** @fd: dma-buf file descriptor (in) */
	__s32 fd;
	/** @count: Count of DMA addresses in the dma-buf (out) */
	__u32 count;
	/** @bdf: PCI device to import for, e.g. "0000:01:00.0" (in) */
	char bdf[DMABUF_IMPORT_BDF_LEN];
};

/**
 * struct dmabuf_import_info - Where the imported memory actually lives
 *
 * A dma-buf exporter may satisfy an import by migrating the buffer to system
 * memory instead of handing out the device's own, which succeeds while quietly
 * not being peer-to-peer at all. The kernel knows which it did: a segment
 * carrying a PCI bus address is the device's memory, reached over the bus.
 *
 * @nbus == @count is an import that stayed on the device; @nbus == 0 is one
 * that did not. Anything between is a partial migration.
 */
struct dmabuf_import_info {
	/** @fd: dma-buf file descriptor (in) */
	__s32 fd;
	/** @count: Segments in the mapping (out) */
	__u32 count;
	/** @nbus: Segments whose DMA address is a PCI bus address (out) */
	__u32 nbus;
	/** @pad: Must be zero */
	__u32 pad;
};

#define DMABUF_IMPORT_ATTACH  _IOWR('u', 0x47, struct dmabuf_import_attach)
#define DMABUF_IMPORT_DETACH  _IOW('u', 0x48, int)
#define DMABUF_IMPORT_GET_MAP _IOWR('u', 0x49, struct dmabuf_import_get_map)
#define DMABUF_IMPORT_GET_INFO _IOWR('u', 0x4a, struct dmabuf_import_info)
#define DMABUF_IMPORT_ATTACH_BDF _IOWR('u', 0x4b, struct dmabuf_import_attach_bdf)

#endif /* _UAPI_LINUX_DMABUF_IMPORT_H */
