// SPDX-License-Identifier: BSD-3-Clause

/**
 * Get physical addresses from a dma-buf
 * =====================================
 *
 * This is a generic interface that is compatible with any dma-buf.
 * A dma-buf FD can be obtained either from host memory, memfd->udmabuf, or
 * device memory, e.g., CUDA or ROCm.
 *
 * NOTE: The import path uses the out-of-tree dmabuf_import module, which serves
 * its ioctls on /dev/dmabuf_import. Its UAPI is <linux/dmabuf_import.h>,
 * installed by the dmabuf-import DKMS package, which ships with the upcie
 * release as an asset but is versioned independently. That header is optional:
 * when it is not available, dmabuf_attach()/dmabuf_detach() compile as stubs
 * returning -ENOTSUP, so upcie builds and runs without the module (the import
 * calls simply fail). Install the package to enable the path; the module must
 * also be loaded at runtime.
 *
 * @file dmabuf.h
 * @version 0.5.2
 */

#include <linux/dma-buf.h>

/* Optional: pull in the dmabuf_import UAPI if the DKMS package installed it.
 * Guarded with __has_include so upcie builds without it. */
#if defined(__has_include)
#  if __has_include(<linux/dmabuf_import.h>)
#    include <linux/dmabuf_import.h>
#  endif
#endif

struct dmabuf_page {
	uint64_t addr;			///< Address of a page
	uint64_t len;			///< Length of the page (can span multiple phys pages)
};

struct dmabuf {
	int fd;				///< dma-buf file descriptor
	size_t npages;			///< Number of pages in the dma-buf
	struct dmabuf_page *pages;	///< Array of pages in the dma-buf
};

/**
 * Print information about the given dma-buf and each of it's pages
 */
static inline int
dmabuf_pp(struct dmabuf *dmabuf)
{
	int wrtn = 0;

	wrtn += printf("dmabuf:");

	if (!dmabuf) {
		wrtn += printf(" ~\n");
		return 0;
	}

	wrtn += printf("\n");
	wrtn += printf("  fd: %d\n", dmabuf->fd);
	wrtn += printf("  npages: %zu\n", dmabuf->npages);
	wrtn += printf("  pages:\n");
	for (size_t i = 0; i < dmabuf->npages; ++i) {
		struct dmabuf_page page = dmabuf->pages[i];
		wrtn += printf("  - addr: 0x%" PRIx64 ", len: %" PRIu64 "\n", page.addr, page.len);
	}

	return wrtn;
}

/**
 * Get LUT (lookup table) from dma-buf
 *
 * The pages in the dma-buf might span multiple physical pages.
 * This function creates a LUT segmented to fit the provided page_size.
 *
 * NOTE: Requires pre-allocated phys_lut
 */
static inline int
dmabuf_get_lut(struct dmabuf *dmabuf, size_t nphys, uint64_t *phys_lut, uint64_t page_size)
{
	size_t i = 0;

	for (uint32_t j = 0; j < dmabuf->npages; j++) {
		// handle a single address for multiple pages
		for (uint64_t k = 0; k < dmabuf->pages[j].len / page_size; k++) {
			if (i >= nphys) {
				UPCIE_DEBUG("FAILED: dmabuf (%zu) has more pages than expected (%zu)", i, nphys);
				return -EINVAL;
			}

			phys_lut[i] = dmabuf->pages[j].addr + k * page_size;
			i++;
		}
	}

	if (i != nphys) {
		UPCIE_DEBUG("FAILED: LUT is not full: actual < expected (%zu < %zu)", i, nphys);
		return -EINVAL;
	}

	return 0;
}

#ifdef DMABUF_IMPORT_ATTACH
/**
 * Attach to dma-buf with given FD
 *
 * Populates the given dma-buf structure with information about the dma-buf.
 */
static inline int
dmabuf_attach(int dmabuf_fd, struct dmabuf *dmabuf)
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
dmabuf_detach(struct dmabuf *dmabuf)
{
	int import_fd, err;

	free(dmabuf->pages);

	import_fd = open(DMABUF_IMPORT_DEVPATH, O_RDWR);
	if (import_fd < 0) {
		err = -errno;
		UPCIE_DEBUG("FAILED: open(%s), errno: %d", DMABUF_IMPORT_DEVPATH, err);
		return err;
	}

	err = ioctl(import_fd, DMABUF_IMPORT_DETACH, &dmabuf->fd);
	if (err) {
		err = -errno;
		UPCIE_DEBUG("FAILED: ioctl(DMABUF_IMPORT_DETACH), errno: %d\n", err);
		// fall-through
	}

	close(dmabuf->fd);
	close(import_fd);
	return err;
}
#else /* !DMABUF_IMPORT_ATTACH: dmabuf-import UAPI unavailable, provide stubs */
/**
 * Attach stub -- the dmabuf_import UAPI (<linux/dmabuf_import.h>) is not
 * available. Install the dmabuf-import DKMS package to enable importing.
 */
static inline int
dmabuf_attach(int UPCIE_UNUSED(dmabuf_fd), struct dmabuf *UPCIE_UNUSED(dmabuf))
{
	UPCIE_DEBUG("FAILED: dmabuf_import unavailable; install dmabuf-import-dkms");
	return -ENOTSUP;
}

static inline int
dmabuf_detach(struct dmabuf *UPCIE_UNUSED(dmabuf))
{
	UPCIE_DEBUG("FAILED: dmabuf_import unavailable; install dmabuf-import-dkms");
	return -ENOTSUP;
}
#endif /* DMABUF_IMPORT_ATTACH */
