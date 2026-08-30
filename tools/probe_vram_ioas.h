// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * VRAM into an IOAS: will the kernel map GPU memory for a peer to DMA against?
 * ===========================================================================
 *
 * Under uio_pci_generic a controller consumes physical addresses, and a GPU
 * allocation reaches it through the dma-buf scatter list. Under vfio-pci it
 * consumes IOVAs, so the same allocation has to enter the IOAS, which means
 * IOMMU_IOAS_MAP_FILE has to accept a dma-buf exported by the GPU runtime.
 *
 * iommufd.h records that as of 6.19 it does not, accepting only dma-bufs
 * exported by vfio-pci. That is a comment about a kernel we no longer run, and
 * the answer decides whether a controller under an IOMMU can DMA into VRAM at
 * all, which is upstream of every question about sharing one between
 * processes. So it is asked rather than assumed.
 *
 * A memfd is mapped first as a control, since a MAP_FILE that refuses
 * everything would say nothing about GPU memory in particular.
 *
 * The flavour supplies vram_ioas_probe_{rt_init,alloc_export}, then calls
 * vram_ioas_probe_run().
 *
 * @file probe_vram_ioas.h
 */

#include <upcie/upcie.h>

static void
vram_ioas_probe_say(const char *what, const char *how)
{
	printf("  %-34s %s\n", what, how);
}

static int
vram_ioas_probe_run(size_t nbytes)
{
	struct iommufd iommufd = {0};
	uint64_t iova = 0;
	int dmabuf_fd = -1;
	int memfd = -1;
	int err;

	printf("vram_ioas_probe: %zu bytes\n", nbytes);

	err = iommufd_open(&iommufd);
	if (err) {
		vram_ioas_probe_say("iommufd_open()", strerror(-err));
		return err;
	}

	err = iommufd_ioas_alloc(&iommufd);
	if (err) {
		vram_ioas_probe_say("iommufd_ioas_alloc()", strerror(-err));
		goto out;
	}
	vram_ioas_probe_say("iommufd_ioas_alloc()", "ok");

	memfd = memfd_create("vram_ioas_probe", 0);
	if (memfd < 0 || ftruncate(memfd, (off_t)nbytes)) {
		vram_ioas_probe_say("memfd_create()", strerror(errno));
		err = -errno;
		goto out;
	}

	err = iommufd_ioas_map_file(&iommufd, memfd, 0, nbytes,
				    IOMMU_IOAS_MAP_READABLE | IOMMU_IOAS_MAP_WRITEABLE, &iova);
	if (err) {
		printf("  %-34s %s\n", "MAP_FILE(memfd) [control]", strerror(-err));
	} else {
		printf("  %-34s ok, iova=0x%" PRIx64 "\n", "MAP_FILE(memfd) [control]", iova);
	}

	err = vram_ioas_probe_rt_init();
	if (err) {
		vram_ioas_probe_say("GPU runtime init", "FAILED");
		goto out;
	}

	err = vram_ioas_probe_alloc_export(nbytes, &dmabuf_fd);
	if (err) {
		printf("  %-34s FAILED rc(%d)\n", "alloc and export as dma-buf", err);
		goto out;
	}
	vram_ioas_probe_say("alloc and export as dma-buf", "ok");

	iova = 0;
	err = iommufd_ioas_map_file(&iommufd, dmabuf_fd, 0, nbytes,
				    IOMMU_IOAS_MAP_READABLE | IOMMU_IOAS_MAP_WRITEABLE, &iova);
	if (err) {
		printf("  %-34s %s\n", "MAP_FILE(GPU dma-buf)", strerror(-err));
		printf("\nVerdict: a controller behind an IOMMU cannot DMA into this "
		       "memory.\n");
	} else {
		printf("  %-34s ok, iova=0x%" PRIx64 "\n", "MAP_FILE(GPU dma-buf)", iova);
		printf("\nVerdict: GPU memory enters the IOAS; peer DMA into VRAM is "
		       "available under vfio.\n");
	}

out:
	if (dmabuf_fd >= 0) {
		close(dmabuf_fd);
	}
	if (memfd >= 0) {
		close(memfd);
	}
	iommufd_close(&iommufd);

	return 0;
}
