/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_DMABUF_IMPORT_H
#define _UAPI_LINUX_DMABUF_IMPORT_H

#include <linux/types.h>
#include <linux/ioctl.h>

/*
 * UAPI for the out-of-tree dma-buf importer (/dev/dmabuf_import).
 *
 * These ioctls originally lived as a patch on the in-tree, built-in
 * udmabuf driver (CONFIG_UDMABUF=y), which forced a full kernel rebuild.
 * They are additive + self-contained (import an external dma-buf and hand
 * its DMA addresses to userspace, using only exported dma-buf core APIs),
 * so they are lifted here into a standalone module with its own device.
 * Nothing here is udmabuf-specific: any dma-buf descriptor can be imported,
 * whoever exported it.
 *
 * The ioctl magic ('u') + numbers match that original patch, which is the
 * only reason they look the way they do. Because they are issued on a
 * SEPARATE fd (/dev/dmabuf_import), they never clash with the stock
 * /dev/udmabuf UDMABUF_CREATE / UDMABUF_CREATE_LIST ioctls.
 *
 * Attachments are owned by the /dev/dmabuf_import descriptor that created
 * them: DMABUF_IMPORT_GET_MAP and DMABUF_IMPORT_DETACH must be issued on the
 * same descriptor as the DMABUF_IMPORT_ATTACH, and closing it tears the
 * attachment down. Keep it open for as long as the DMA addresses are in use.
 *
 * Installed system-wide as <linux/dmabuf_import.h> by the dmabuf-import-dkms
 * package.
 */

/*
 * Device node the importer module registers (miscdevice "dmabuf_import").
 * Open this for the import ioctls; override with -DDMABUF_IMPORT_DEVPATH=...
 */
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

#define DMABUF_IMPORT_ATTACH  _IOWR('u', 0x47, struct dmabuf_import_attach)
#define DMABUF_IMPORT_DETACH  _IOW('u', 0x48, int)
#define DMABUF_IMPORT_GET_MAP _IOWR('u', 0x49, struct dmabuf_import_get_map)

#endif /* _UAPI_LINUX_DMABUF_IMPORT_H */
