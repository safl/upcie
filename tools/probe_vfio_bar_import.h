// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * vfio BAR dma-buf import probe: will a GPU runtime map another device's MMIO?
 * ===========================================================================
 *
 * VFIO_DEVICE_FEATURE_DMA_BUF exports a slice of a device's BAR as a dma-buf.
 * The exporter does not implement CPU mmap, so a process holding only that
 * descriptor cannot produce a host address, and cuMemHostRegister() with
 * CU_MEMHOSTREGISTER_IOMEMORY, which is how the GPU-initiated NVMe path
 * reaches a doorbell today, has nothing to register.
 *
 * The question this settles is whether the other route exists: does the GPU
 * runtime import the descriptor itself and hand back a device pointer? If it
 * does, MMIO can be delegated to a process that never holds the device fd. If
 * it does not, delegating the doorbells means delegating the device.
 *
 * Reads rather than writes, and defaults to offset 0, so what it touches is
 * CAP rather than a doorbell: the host reads the same register through its own
 * mapping and the two are printed side by side. A device-side value equal to
 * the host's is the whole result.
 *
 * The flavour supplies
 * bar_import_probe_{rt_init,selftest,import,read,release,why}, then calls
 * bar_import_probe_run(). The self-import is a control: a runtime that will not
 * import a descriptor it exported itself is not telling us anything about
 * vfio.
 *
 * @file probe_vfio_bar_import.h
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <upcie/upcie.h>

struct bar_import_probe_args {
	const char *cdev; ///< /dev/vfio/devices/vfioN
	uint32_t region;  ///< BAR/region index
	uint64_t offset;  ///< Offset within the region
	uint64_t length;  ///< Length of the exported slice
};

static void
bar_import_probe_say(const char *what, const char *how)
{
	printf("  %-34s %s\n", what, how);
}

static int
bar_import_probe_run(const struct bar_import_probe_args *args)
{
	struct vfio_region_info region = {.index = args->region};
	struct iommufd iommufd = {0};
	struct vfio_cdev dev = {0};
	uint64_t devptr = 0;
	uint32_t host_lo = 0, host_hi = 0, dev_lo = 0, dev_hi = 0;
	uint32_t pair[2] = {0};
	void *host_bar = NULL;
	int dbuf_fd = -1;
	int err;

	printf("vfio_bar_import_probe: %s region=%u offset=0x%" PRIx64 " length=0x%" PRIx64 "\n",
	       args->cdev, args->region, args->offset, args->length);

	err = iommufd_open(&iommufd);
	if (err) {
		bar_import_probe_say("iommufd_open()", strerror(-err));
		return err;
	}

	err = iommufd_ioas_alloc(&iommufd);
	if (err) {
		bar_import_probe_say("iommufd_ioas_alloc()", strerror(-err));
		goto out_iommufd;
	}

	err = vfio_cdev_open(args->cdev, &dev);
	if (err) {
		bar_import_probe_say("vfio_cdev_open()", strerror(-err));
		goto out_ioas;
	}

	err = iommufd_bind(&iommufd, &dev);
	if (err) {
		bar_import_probe_say("iommufd_bind()", strerror(-err));
		goto out_dev;
	}

	err = iommufd_attach(&iommufd, &dev);
	if (err) {
		bar_import_probe_say("iommufd_attach()", strerror(-err));
		goto out_dev;
	}
	bar_import_probe_say("bind and attach", "ok");

	if (vfio_device_get_region_info(dev.fd, &region)) {
		bar_import_probe_say("GET_REGION_INFO", strerror(errno));
		err = -errno;
		goto out_detach;
	}
	printf("  %-34s size=0x%llx mmap=%s\n", "region", (unsigned long long)region.size,
	       (region.flags & VFIO_REGION_INFO_FLAG_MMAP) ? "yes" : "no");

	host_bar = vfio_map_region(dev.fd, region.size, region.offset);
	if (host_bar == MAP_FAILED) {
		host_bar = NULL;
		bar_import_probe_say("host mmap(region)", strerror(errno));
	} else {
		host_lo = mmio_read32(host_bar, args->offset);
		host_hi = mmio_read32(host_bar, args->offset + 4);
		printf("  %-34s 0x%08" PRIx32 " 0x%08" PRIx32 "\n", "host read", host_lo, host_hi);
	}

	dbuf_fd = vfio_device_bar_export_dmabuf(dev.fd, args->region, args->offset, args->length);
	if (dbuf_fd < 0) {
		bar_import_probe_say("EXPORT_DMA_BUF", strerror(-dbuf_fd));
		err = dbuf_fd;
		goto out_unmap;
	}
	bar_import_probe_say("EXPORT_DMA_BUF", "ok");

	err = bar_import_probe_rt_init();
	if (err) {
		bar_import_probe_say("GPU runtime init", "FAILED");
		goto out_dmabuf;
	}

	{
		char why[128] = {0};

		err = bar_import_probe_selftest(why, sizeof(why));
		bar_import_probe_say(err ? "self-import (control)" : "self-import (control) ok",
				     why);
	}

	/* The runtime takes ownership of the descriptor on import, so it is not
	 * closed here even on the failure paths below. */
	err = bar_import_probe_import(dbuf_fd, args->length, &devptr);
	if (err) {
		printf("  %-34s FAILED (%s)\n", "import into GPU runtime", bar_import_probe_why());
		printf("\nVerdict: MMIO cannot be delegated by descriptor to this runtime.\n");
		goto out_release;
	}
	printf("  %-34s ok, devptr=0x%" PRIx64 "\n", "import into GPU runtime", devptr);

	err = bar_import_probe_read(devptr, sizeof(pair), pair);
	if (err) {
		printf("  %-34s FAILED rc(%d)\n", "device-side read", err);
		printf("\nVerdict: imported, but the mapping is not readable from the device.\n");
		goto out_release;
	}
	dev_lo = pair[0];
	dev_hi = pair[1];
	printf("  %-34s 0x%08" PRIx32 " 0x%08" PRIx32 "\n", "device-side read", dev_lo, dev_hi);

	if (host_bar && dev_lo == host_lo && dev_hi == host_hi) {
		printf("\nVerdict: the device reads what the host reads; MMIO can be "
		       "delegated by descriptor.\n");
	} else if (host_bar) {
		printf("\nVerdict: the mapping resolved but does not read as the host's; "
		       "not usable as it stands.\n");
	} else {
		printf("\nVerdict: imported and read, with no host mapping to compare "
		       "against.\n");
	}

out_release:
	bar_import_probe_release();
	goto out_unmap;

out_dmabuf:
	close(dbuf_fd);
out_unmap:
	if (host_bar) {
		munmap(host_bar, region.size);
	}
out_detach:
	iommufd_detach(&dev);
out_dev:
	vfio_cdev_close(&dev);
out_ioas:
	iommufd_destroy(&iommufd, iommufd.ioas_id);
out_iommufd:
	iommufd_close(&iommufd);

	return err;
}

static int
bar_import_probe_main(int argc, char *argv[])
{
	struct bar_import_probe_args args = {.region = 0, .offset = 0, .length = 0x1000};

	if (argc < 2 || argc > 5) {
		fprintf(stderr, "usage: %s <cdev> [region] [offset] [length]\n", argv[0]);
		return 2;
	}

	args.cdev = argv[1];
	if (argc > 2) {
		args.region = (uint32_t)strtoul(argv[2], NULL, 0);
	}
	if (argc > 3) {
		args.offset = (uint64_t)strtoull(argv[3], NULL, 0);
	}
	if (argc > 4) {
		args.length = (uint64_t)strtoull(argv[4], NULL, 0);
	}

	bar_import_probe_run(&args);

	return EXIT_SUCCESS;
}
