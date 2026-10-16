// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * dma-buf export of a memfd dmamem through udmabuf
 * ================================================
 *
 * A range of a memfd-backed dmamem handed to others as a dma-buf, which
 * /dev/udmabuf makes of a memfd's pages: for memory a device emulated in
 * software keeps in a memfd, exported as a card exports VRAM, and mapped or
 * imported by whoever receives the fd. udmabuf takes only a memfd sealed
 * against shrinking and not against writes, which dmamem_from_memfd() makes
 * without an iommufd; the range must be page aligned.
 *
 * The dma-buf holds the pages; the dmamem may be destroyed while it lives.
 *
 * @file dmamem_udmabuf.h
 * @version 0.9.0
 */

/**
 * Make a dma-buf of a range of a memfd-backed dmamem.
 *
 * @param dmem    A dmamem holding a memfd (backing DMAMEM_BACKING_MEMFD).
 * @param offset  Page-aligned offset of the range in the dmamem.
 * @param size    Page-aligned size of the range.
 * @param fd      The dma-buf's fd on success, the caller's to close; -1
 *                otherwise.
 *
 * @return 0 on success, negative errno on error: -ENODEV where the kernel has
 * no /dev/udmabuf, -EINVAL for a range or a memfd udmabuf does not take.
 */
static inline int
dmamem_export_udmabuf(const struct dmamem *dmem, uint64_t offset, size_t size, int *fd)
{
	struct udmabuf_create create = {0};
	int dev, err = 0;

	*fd = -1;
	if (!dmem || dmem->fd < 0 || dmem->backing != DMAMEM_BACKING_MEMFD || !size ||
	    offset > dmem->size || size > dmem->size - offset) {
		return -EINVAL;
	}

	dev = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
	if (dev < 0) {
		err = errno == ENOENT ? -ENODEV : -errno;
		UPCIE_DEBUG("FAILED: open(/dev/udmabuf); errno(%d)", errno);
		return err;
	}

	create.memfd = (uint32_t)dmem->fd;
	create.flags = UDMABUF_FLAGS_CLOEXEC;
	create.offset = offset;
	create.size = size;
	*fd = ioctl(dev, UDMABUF_CREATE, &create);
	if (*fd < 0) {
		err = -errno;
		*fd = -1;
		UPCIE_DEBUG("FAILED: ioctl(UDMABUF_CREATE); errno(%d)", -err);
	}

	close(dev);
	return err;
}
