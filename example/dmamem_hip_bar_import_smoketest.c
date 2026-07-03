// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * NVMe BAR (as vfio-pci dma-buf) imported into HIP
 * ================================================
 *
 * The "decisive test" from the GINA notebook's AMD prototype: can the
 * HIP-side dma-buf importer (hipImportExternalMemory +
 * hipExternalMemoryGetMappedBuffer) accept a dma-buf produced by
 * vfio-pci's VFIO_DEVICE_FEATURE_DMA_BUF export? If yes, an amdgpu-driven
 * GPU can address the NVMe BAR through a normal HIP device pointer and
 * ring the doorbell from a kernel without the CPU-relayed bridge.
 *
 * Setup expected on the target:
 *  - NVMe bound to vfio-pci (so VFIO_DEVICE_FEATURE_DMA_BUF works).
 *  - AMD GPU bound to amdgpu (compute-active) so HIP can enumerate it.
 *
 * Usage:
 *   dmamem_hip_bar_import_smoketest <nvme_cdev> <bar_index> <offset> <length>
 *   e.g. wave, doorbell page:
 *   dmamem_hip_bar_import_smoketest /dev/vfio/devices/vfio0 0 0x1000 0x1000
 */
#define _GNU_SOURCE
#include <upcie/upcie.h>
#include <hip/hip_runtime_api.h>

static const char *
hip_err_name(hipError_t e)
{
	const char *n = hipGetErrorName(e);
	return n ? n : "?";
}

int
main(int argc, char *argv[])
{
	struct iommufd iommufd = {0};
	struct iommufd_device dev = {0};
	uint32_t ioas_id = 0;
	uint32_t bar_index;
	uint64_t offset, length;
	int dbuf_fd = -1;
	int err;
	int hip_device_count = 0;
	hipError_t herr;

	if (argc != 5) {
		fprintf(stderr,
			"usage: %s <nvme_cdev> <bar_index> <offset> <length>\n"
			"  e.g. %s /dev/vfio/devices/vfio0 0 0x1000 0x1000\n",
			argv[0], argv[0]);
		return 2;
	}
	bar_index = (uint32_t)strtoul(argv[2], NULL, 0);
	offset = (uint64_t)strtoull(argv[3], NULL, 0);
	length = (uint64_t)strtoull(argv[4], NULL, 0);

	herr = hipGetDeviceCount(&hip_device_count);
	if (herr != hipSuccess) {
		fprintf(stderr, "FAIL: hipGetDeviceCount() -> %s(%d)\n", hip_err_name(herr), herr);
		return 1;
	}
	printf("hip devices: %d\n", hip_device_count);
	if (hip_device_count < 1) {
		fprintf(stderr, "FAIL: no HIP device visible; is the GPU on amdgpu?\n");
		return 1;
	}
	{
		hipDeviceProp_t props = {0};
		hipGetDeviceProperties(&props, 0);
		printf("hip device 0: %s (gcnArchName=%s)\n", props.name, props.gcnArchName);
	}

	err = iommufd_open(&iommufd);
	if (err) {
		fprintf(stderr, "FAIL: iommufd_open err(%d)\n", err);
		return 1;
	}
	err = iommufd_ioas_alloc(&iommufd, &ioas_id);
	if (err) {
		fprintf(stderr, "FAIL: iommufd_ioas_alloc err(%d)\n", err);
		goto out_iommufd;
	}
	err = iommufd_device_open(argv[1], &dev);
	if (err) {
		fprintf(stderr, "FAIL: iommufd_device_open('%s') err(%d)\n", argv[1], err);
		goto out_ioas;
	}
	err = iommufd_device_bind(&dev, &iommufd);
	if (err) {
		fprintf(stderr, "FAIL: iommufd_device_bind err(%d)\n", err);
		goto out_dev;
	}
	err = iommufd_device_attach(&dev, ioas_id);
	if (err) {
		fprintf(stderr, "FAIL: iommufd_device_attach err(%d)\n", err);
		goto out_dev;
	}

	dbuf_fd = vfio_device_bar_export_dmabuf(dev.fd, bar_index, offset, length);
	if (dbuf_fd < 0) {
		fprintf(stderr, "FAIL: vfio_device_bar_export_dmabuf err(%d): %s\n", dbuf_fd,
			strerror(-dbuf_fd));
		err = dbuf_fd;
		goto out_detach;
	}
	printf("bar dma-buf: fd=%d region=%u offset=0x%" PRIx64 " length=0x%" PRIx64 "\n", dbuf_fd,
	       bar_index, offset, length);

	{
		hipExternalMemory_t ext = NULL;
		hipExternalMemoryHandleDesc mem_desc = {0};

		mem_desc.type = hipExternalMemoryHandleTypeOpaqueFd;
		mem_desc.handle.fd = dbuf_fd;
		mem_desc.size = length;
		mem_desc.flags = 0;

		herr = hipImportExternalMemory(&ext, &mem_desc);
		if (herr != hipSuccess) {
			fprintf(stderr,
				"HIP import: FAIL hipImportExternalMemory -> %s(%d)\n"
				"       This is the 'does amdgpu accept vfio-pci-exported\n"
				"       dma-bufs' question. Non-success means amdgpu's dma-buf\n"
				"       importer either does not recognise the vfio-pci ops\n"
				"       vector or does not support P2P attach to the exporter.\n",
				hip_err_name(herr), herr);
			close(dbuf_fd);
			err = -EIO;
			goto out_detach;
		}
		printf("HIP import: OK (hipImportExternalMemory)\n");

		void *dptr = NULL;
		hipExternalMemoryBufferDesc buf_desc = {0};
		buf_desc.offset = 0;
		buf_desc.size = length;
		buf_desc.flags = 0;

		herr = hipExternalMemoryGetMappedBuffer(&dptr, ext, &buf_desc);
		if (herr != hipSuccess) {
			fprintf(stderr,
				"HIP map: FAIL hipExternalMemoryGetMappedBuffer -> %s(%d)\n",
				hip_err_name(herr), herr);
			hipDestroyExternalMemory(ext);
			close(dbuf_fd);
			err = -EIO;
			goto out_detach;
		}
		printf("HIP map: OK dptr=%p length=0x%" PRIx64 "\n", dptr, length);

		/* A tiny probe: try a CPU-side hipMemcpy write of a marker word to
		 * the doorbell offset. This does not actually ring the NVMe (we
		 * are not passing a real SQ tail), but if it returns hipSuccess
		 * the GPU-side address really is a valid device address. */
		{
			uint32_t marker = 0;
			herr = hipMemcpy(dptr, &marker, sizeof(marker), hipMemcpyHostToDevice);
			printf("HIP memcpy H2D 4B to doorbell offset: %s(%d)\n",
			       hip_err_name(herr), herr);
		}

		hipDestroyExternalMemory(ext);
	}

	close(dbuf_fd);
	printf("OK: NVMe BAR range imported into HIP via vfio-pci dma-buf export\n");

out_detach:
	iommufd_device_detach(&dev);
out_dev:
	iommufd_device_close(&dev);
out_ioas:
	iommufd_destroy(&iommufd, ioas_id);
out_iommufd:
	iommufd_close(&iommufd);
	return err ? 1 : 0;
}
