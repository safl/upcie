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
 * Attachments made with DMABUF_IMPORT_ATTACH_BDF are owned by the descriptor
 * that created them: issue GET_MAP and DETACH on that same one, and closing it
 * tears the attachment down. Keep it open while the DMA addresses are in use.
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
 * The dma-buf fd must name an import made with either attach ioctl: one made
 * with DMABUF_IMPORT_ATTACH_BDF is found only on the descriptor it was made
 * on, one made with DMABUF_IMPORT_ATTACH on any descriptor. The dma_arr array
 * is allocated by userspace.
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
 * @nbus counts segments the kernel's own P2PDMA framework mapped, which it
 * records by giving them a PCI bus address rather than a host one. That is
 * narrower than "this is device memory": the framework only engages for a PCI
 * importer, and an exporter reaching its own BAR by other means sets nothing,
 * so @nbus reads zero for memory that is plainly on the device. It says how
 * the mapping was made, not where the memory is; DMABUF_IMPORT_DESCRIBE
 * answers the latter.
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

/**
 * struct dmabuf_import_describe - What an import actually turned into
 *
 * Everything here is a separate way of asking the same question, because no
 * one of them answers it on its own. @nbus is set only where the kernel's own
 * P2PDMA framework did the mapping, so it reads zero for device memory reached
 * any other way. @nopage counts segments with no struct page behind them,
 * which host memory never is. @exporter and @importer say who was asked and on
 * whose behalf, since the answer depends on both.
 *
 * Added alongside DMABUF_IMPORT_GET_INFO rather than replacing it: an ioctl
 * that changes what it reports is worse than a second one that reports more.
 */
struct dmabuf_import_describe {
	/** @fd: dma-buf file descriptor (in) */
	__s32 fd;
	/** @count: Segments in the mapping (out) */
	__u32 count;
	/** @nbus: Segments whose DMA address is a PCI bus address (out) */
	__u32 nbus;
	/** @nopage: Entries with no struct page behind them (out) */
	__u32 nopage;
	/** @npages: Entries on the CPU-side list, which @nopage counts against (out) */
	__u32 npages;
	/** @pinned: Whether the attachment is pinned (out) */
	__u32 pinned;
	/** @nbytes: Total bytes mapped (out) */
	__u64 nbytes;
	/** @exporter: The exporting driver, e.g. "amdgpu" (out) */
	char exporter[32];
	/** @importer: Device the attachment was made as, or "misc" (out) */
	char importer[DMABUF_IMPORT_BDF_LEN];
};

/*
 * DMABUF_IMPORT_ATTACH is kept for callers that have not moved, and is the one
 * to move off. It keys its imports by the dma-buf descriptor number in a table
 * shared by every user of the device, and holds them until a DETACH arrives. A
 * descriptor number means something only inside the process that holds it, so
 * two processes naming different buffers with the same number collide. Such a
 * collision used to be answered with whichever buffer got there first; it is
 * now refused with -ESTALE, a change made knowingly: an error can be handled
 * where wrong DMA addresses cannot, and the only attach refused is one whose
 * number no longer names the buffer the standing import holds. A DETACH of
 * that import frees the number. A process that is killed still sends no
 * DETACH, so its import holds the exporter's memory until the module is
 * unloaded. Taking this ioctl says so in the kernel log.
 *
 * DMABUF_IMPORT_ATTACH_BDF hands the import to the file it was made on. Each
 * open of the device has its own, so the numbers cannot meet, and closing the
 * file gives the imports back, which the kernel does on exit however the exit
 * happens. The caller therefore keeps the device open for as long as it uses
 * the addresses. An empty bdf attaches as the misc device, which is what
 * ATTACH does, so it is the whole of what ATTACH offered and none of what it
 * got wrong.
 *
 * The lookups take an import made either way: they consult the calling file's
 * imports before the shared table. An import whose descriptor number has since
 * come to name a different dma-buf is refused with -ESTALE wherever it is
 * found; only DETACH still takes it, which is how the number is freed.
 */
#define DMABUF_IMPORT_ATTACH  _IOWR('u', 0x47, struct dmabuf_import_attach)
#define DMABUF_IMPORT_DETACH  _IOW('u', 0x48, int)
#define DMABUF_IMPORT_GET_MAP _IOWR('u', 0x49, struct dmabuf_import_get_map)
#define DMABUF_IMPORT_GET_INFO _IOWR('u', 0x4a, struct dmabuf_import_info)
#define DMABUF_IMPORT_ATTACH_BDF _IOWR('u', 0x4b, struct dmabuf_import_attach_bdf)
#define DMABUF_IMPORT_DESCRIBE _IOWR('u', 0x4c, struct dmabuf_import_describe)

#endif /* _UAPI_LINUX_DMABUF_IMPORT_H */
