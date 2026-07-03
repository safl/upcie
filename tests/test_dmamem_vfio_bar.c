// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * dmamem_from_dmabuf via VFIO_DEVICE_FEATURE_DMA_BUF smoketest
 * ============================================================
 *
 * The mainline-accepted dma-buf path: bind a vfio-pci device to iommufd,
 * ask VFIO to export a slice of one of its BAR regions as a dma-buf, and
 * pass that dma-buf fd to iommufd's IOMMU_IOAS_MAP_FILE via
 * dmamem_from_dmabuf. As of Linux 6.19 the vfio-pci exporter is the only
 * one iommufd's MAP_FILE accepts, so this is the current mainline test
 * that a dma-buf + iommufd import actually works end to end.
 *
 * Usage:
 *   test_dmamem_vfio_bar <cdev> <region_index> <length>
 *   e.g.
 *   test_dmamem_vfio_bar /dev/vfio/devices/vfio0 1 0x40000000
 *
 * On success prints the IOVA the kernel picked plus the dma-buf fd. On
 * failure the errno at the ioctl surfaces exactly.
 */
#include <upcie/upcie.h>

int
main(int argc, char *argv[])
{
	struct iommufd iommufd = {0};
	struct vfio_cdev dev = {0};
	struct dmamem dmem = {0};
	uint32_t region_index;
	uint64_t length;
	int dbuf_fd = -1;
	int err;

	if (argc != 4) {
		fprintf(stderr, "usage: %s <cdev> <region_index> <length>\n", argv[0]);
		return 2;
	}
	region_index = (uint32_t)strtoul(argv[2], NULL, 0);
	length = (uint64_t)strtoull(argv[3], NULL, 0);

	err = iommufd_open(&iommufd);
	if (err) {
		fprintf(stderr, "FAIL: iommufd_open() err(%d): %s\n", err, strerror(-err));
		return 1;
	}

	err = iommufd_ioas_alloc(&iommufd);
	if (err) {
		fprintf(stderr, "FAIL: iommufd_ioas_alloc() err(%d): %s\n", err, strerror(-err));
		goto out_iommufd;
	}

	err = vfio_cdev_open(argv[1], &dev);
	if (err) {
		fprintf(stderr, "FAIL: vfio_cdev_open('%s') err(%d): %s\n", argv[1], err,
			strerror(-err));
		goto out_ioas;
	}

	err = iommufd_bind(&iommufd, &dev);
	if (err) {
		fprintf(stderr, "FAIL: iommufd_bind() err(%d): %s\n", err, strerror(-err));
		goto out_dev;
	}

	err = iommufd_attach(&iommufd, &dev);
	if (err) {
		fprintf(stderr, "FAIL: iommufd_attach() err(%d): %s\n", err,
			strerror(-err));
		goto out_dev;
	}

	dbuf_fd = vfio_device_bar_export_dmabuf(dev.fd, region_index, 0, length);
	if (dbuf_fd < 0) {
		fprintf(stderr, "FAIL: vfio_device_bar_export_dmabuf(region=%u, len=0x%" PRIx64
				") err(%d): %s\n",
			region_index, length, dbuf_fd, strerror(-dbuf_fd));
		err = dbuf_fd;
		goto out_detach;
	}
	printf("BAR dma-buf: fd=%d region=%u length=0x%" PRIx64 "\n", dbuf_fd, region_index,
	       length);

	err = dmamem_from_dmabuf(&dmem, &iommufd, dbuf_fd, length);
	if (err) {
		fprintf(stderr, "FAIL: dmamem_from_dmabuf() err(%d): %s\n", err, strerror(-err));
		close(dbuf_fd);
		goto out_detach;
	}
	dmamem_pp(&dmem);

	printf("OK: base_iova=0x%" PRIx64 " size=%zu cpu_va=%s\n", dmem.base_iova, dmem.size,
	       dmem.cpu_va ? "mapped" : "unavailable");

	dmamem_destroy(&dmem);

out_detach:
	iommufd_detach(&dev);
out_dev:
	vfio_cdev_close(&dev);
out_ioas:
	iommufd_destroy(&iommufd, iommufd.ioas_id);
out_iommufd:
	iommufd_close(&iommufd);
	return err ? 1 : 0;
}
