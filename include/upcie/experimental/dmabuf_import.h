// SPDX-License-Identifier: BSD-3-Clause

/**
 * Resolve a dma-buf's DMA addresses via the dmabuf_import module
 * ==============================================================
 *
 * ==========================================================================
 * EXPERIMENTAL DEPENDENCY
 * Requires the out-of-tree dmabuf-import DKMS module and /dev/dmabuf_import.
 * Without the module's UAPI header these compile to stubs returning -ENOTSUP,
 * so uPCIe still builds; UPCIE_HAVE_DMABUF_IMPORT tells you which case you got.
 * The module must also be loaded at runtime for the import to succeed.
 * ==========================================================================
 *
 * A dma-buf descriptor can be obtained either from host memory (memfd via
 * udmabuf) or from device memory, e.g. CUDA or ROCm. Importing it yields the
 * DMA addresses behind it, which is what raw-physical DMA needs.
 *
 * The UAPI is <linux/dmabuf_import.h>, installed by the dmabuf-import DKMS
 * package, which ships with the upcie release as an asset but is versioned
 * independently.
 *
 * @file dmabuf_import.h
 * @version 0.6.0
 */
#ifndef UPCIE_EXPERIMENTAL_DMABUF_IMPORT_H
#define UPCIE_EXPERIMENTAL_DMABUF_IMPORT_H

/* Include <upcie/dmabuf.h> before this header; it defines struct dmabuf and,
 * like the rest of the uPCIe headers, has no include guard of its own, so it
 * cannot be pulled in from here. <upcie/upcie.h> orders the two correctly. */

/* Optional: pull in the dmabuf_import UAPI if the DKMS package installed it.
 * Guarded with __has_include so upcie builds without it. */
#if defined(__has_include)
#  if __has_include(<linux/dmabuf_import.h>)
#    include <linux/dmabuf_import.h>
#  endif
#endif

#ifdef DMABUF_IMPORT_ATTACH
/* The module's UAPI is available, so the real import path is compiled in. */
#define UPCIE_HAVE_DMABUF_IMPORT 1

/**
 * Attach to dma-buf with given FD
 *
 * Populates the given dma-buf structure with information about the dma-buf.
 *
 * NOTE: The attachment, and thus the validity of the addresses returned here,
 * is owned by the /dev/dmabuf_import descriptor. It is kept open in
 * dmabuf->import_fd until dmabuf_import_detach(); closing it drops the DMA
 * mapping.
 */
static inline int
dmabuf_import_attach(int dmabuf_fd, struct dmabuf *dmabuf)
{
	struct dmabuf_import_attach attach;
	struct dmabuf_import_get_map *map = NULL;
	int import_fd, err;
	size_t map_size, pages_size;

	import_fd = open(DMABUF_IMPORT_DEVPATH, O_RDWR);
	if (import_fd < 0) {
		err = -errno;
		UPCIE_DEBUG("FAILED: open(%s), errno: %d", DMABUF_IMPORT_DEVPATH, err);
		return err;
	}

	memset(&attach, 0, sizeof(attach));
	attach.fd = dmabuf_fd;

	err = ioctl(import_fd, DMABUF_IMPORT_ATTACH, &attach);
	if (err) {
		err = -errno;
		UPCIE_DEBUG("FAILED: ioctl(DMABUF_IMPORT_ATTACH), errno: %d", err);
		goto exit;
	}

	map_size = attach.count * sizeof(struct dmabuf_import_dma_map);
	map = malloc(sizeof(struct dmabuf_import_get_map) + map_size);
	if (!map) {
		err = -errno;
		UPCIE_DEBUG("FAILED: malloc(map), errno: %d", err);
		ioctl(import_fd, DMABUF_IMPORT_DETACH, &dmabuf_fd);
		goto exit;
	}

	memset(map, 0, sizeof(*map));
	map->fd = dmabuf_fd;
	map->count = attach.count;

	err = ioctl(import_fd, DMABUF_IMPORT_GET_MAP, map);
	if (err) {
		err = -errno;
		UPCIE_DEBUG("FAILED: ioctl(DMABUF_IMPORT_GET_MAP), errno: %d\n", err);
		ioctl(import_fd, DMABUF_IMPORT_DETACH, &dmabuf_fd);
		goto exit;
	}

	memset(dmabuf, 0, sizeof(*dmabuf));
	dmabuf->fd = dmabuf_fd;
	dmabuf->import_fd = -1;
	dmabuf->npages = map->count;
	pages_size = sizeof(struct dmabuf_page) * dmabuf->npages;
	dmabuf->pages = malloc(pages_size);
	if (!dmabuf->pages) {
		err = -errno;
		UPCIE_DEBUG("FAILED: malloc(dmabuf->pages), errno: %d", err);
		ioctl(import_fd, DMABUF_IMPORT_DETACH, &dmabuf_fd);
		goto exit;
	}

	memcpy(dmabuf->pages, map->dma_arr, pages_size);

	/* Hand ownership of the importer fd to the caller; the kernel-side
	 * attachment lives exactly as long as this descriptor. */
	dmabuf->import_fd = import_fd;
	free(map);

	return 0;

exit:
	free(map);
	close(import_fd);
	return err;
}

/**
 * Detach from given dma-buf
 *
 * NOTE: This doesn't free the underlying memory
 */
static inline int
dmabuf_import_detach(struct dmabuf *dmabuf)
{
	int err = 0;

	free(dmabuf->pages);
	dmabuf->pages = NULL;

	/* The attachment is scoped to the importer fd that created it, so it
	 * must be torn down through that same fd; closing it would suffice, the
	 * explicit DETACH just makes the teardown ordering visible. */
	/* > 0, not >= 0: a '{0}'-initialised struct that never went through
	 * dmabuf_import_attach() must not make this close descriptor 0. */
	if (dmabuf->import_fd > 0) {
		if (ioctl(dmabuf->import_fd, DMABUF_IMPORT_DETACH, &dmabuf->fd)) {
			err = -errno;
			UPCIE_DEBUG("FAILED: ioctl(DMABUF_IMPORT_DETACH), errno: %d\n", err);
			// fall-through
		}
		close(dmabuf->import_fd);
		dmabuf->import_fd = -1;
	}

	if (dmabuf->fd > 0) {
		close(dmabuf->fd);
		dmabuf->fd = -1;
	}

	return err;
}
#else /* !DMABUF_IMPORT_ATTACH: the module UAPI is unavailable, stub it out */
/* The module's UAPI was not found, so the calls below fail with -ENOTSUP. */
#define UPCIE_HAVE_DMABUF_IMPORT 0

/**
 * Attach stub: the dmabuf_import UAPI (<linux/dmabuf_import.h>) is not
 * available. Install the dmabuf-import DKMS package to enable importing.
 */
static inline int
dmabuf_import_attach(int UPCIE_UNUSED(dmabuf_fd), struct dmabuf *UPCIE_UNUSED(dmabuf))
{
	UPCIE_DEBUG("FAILED: dmabuf_import unavailable; install dmabuf-import-dkms");
	return -ENOTSUP;
}

static inline int
dmabuf_import_detach(struct dmabuf *UPCIE_UNUSED(dmabuf))
{
	UPCIE_DEBUG("FAILED: dmabuf_import unavailable; install dmabuf-import-dkms");
	return -ENOTSUP;
}
#endif /* DMABUF_IMPORT_ATTACH */

#endif /* UPCIE_EXPERIMENTAL_DMABUF_IMPORT_H */
