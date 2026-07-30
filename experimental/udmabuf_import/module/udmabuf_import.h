/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_UDMABUF_IMPORT_H
#define _UAPI_LINUX_UDMABUF_IMPORT_H

#include <linux/types.h>
#include <linux/ioctl.h>

/*
 * UAPI for the out-of-tree udmabuf dma-buf importer (/dev/udmabuf_import).
 *
 * These ioctls originally lived as a patch on the in-tree, built-in
 * udmabuf driver (CONFIG_UDMABUF=y), which forced a full kernel rebuild.
 * They are additive + self-contained (import an external dma-buf and hand
 * its DMA addresses to userspace, using only exported dma-buf core APIs),
 * so they are lifted here into a standalone module with its own device.
 *
 * The ioctl magic ('u') + numbers match the original patch; because they
 * are issued on a SEPARATE fd (/dev/udmabuf_import), they never clash with
 * the stock /dev/udmabuf UDMABUF_CREATE / UDMABUF_CREATE_LIST ioctls.
 */

/*
 * Device node the importer module registers (miscdevice "udmabuf_import").
 * Open this for the import ioctls; override with -DUDMABUF_IMPORT_DEVPATH=...
 */
#ifndef UDMABUF_IMPORT_DEVPATH
#define UDMABUF_IMPORT_DEVPATH "/dev/udmabuf_import"
#endif

/*
 * Coexistence guard: if a patched <linux/udmabuf.h> (the old in-tree delivery)
 * already defined these import ioctls, defer to it. This lets a consumer
 * include this header alongside <linux/udmabuf.h> (e.g. for UDMABUF_CREATE) in
 * one translation unit as long as <linux/udmabuf.h> is seen first.
 */
#ifndef UDMABUF_ATTACH

/**
 * struct udmabuf_attach - import dma-buf and return number of addresses
 */
struct udmabuf_attach {
	/** @fd: dma-buf file descriptor (in) */
	__s32 fd;
	/** @count: Count of DMA addresses in the dma-buf (out) */
	__u32 count;
};

/**
 * struct udmabuf_dma_map - Representation of a DMA mapping
 */
struct udmabuf_dma_map {
	/** @dma_addr: DMA address of the mapping */
	__u64 dma_addr;
	/** @dma_len: Length of the mapping */
	__u64 dma_len;
};

/**
 * struct udmabuf_get_map - Get DMA mappings from the provided dma-buf fd
 *
 * The dma-buf fd must match one previously passed to UDMABUF_ATTACH.
 * The dma_arr array is allocated by userspace.
 */
struct udmabuf_get_map {
	/** @fd: dma-buf file descriptor (in) */
	__s32 fd;
	/** @count: Size of the dma_arr array (in) */
	__u32 count;
	/** @dma_arr: Array of DMA mappings (out) */
	struct udmabuf_dma_map dma_arr[];
};

#define UDMABUF_ATTACH  _IOWR('u', 0x47, struct udmabuf_attach)
#define UDMABUF_DETACH  _IOW('u', 0x48, int)
#define UDMABUF_GET_MAP _IOWR('u', 0x49, struct udmabuf_get_map)

#endif /* UDMABUF_ATTACH */

#endif /* _UAPI_LINUX_UDMABUF_IMPORT_H */
