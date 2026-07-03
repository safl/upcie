// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * NVMe BAR (as vfio-pci dma-buf) imported into amdgpu via libdrm PRIME
 * ===================================================================
 *
 * Companion to dmamem_hip_bar_import_smoketest that goes around HIP and
 * asks amdgpu directly via libdrm_amdgpu's PRIME importer
 * (amdgpu_bo_import with AMDGPU_BO_HANDLE_TYPE_DMA_BUF_FD). If the HIP
 * path rejects the vfio-pci dma-buf but this path accepts it, the gate
 * lives in HIP's higher-level API rather than in amdgpu's dma-buf
 * importer.
 *
 * Usage:
 *   dmamem_libdrm_bar_import_smoketest <nvme_cdev> <bar_index> <offset> <length> [render_node]
 *   e.g. wave, doorbell page:
 *   dmamem_libdrm_bar_import_smoketest /dev/vfio/devices/vfio0 0 0x1000 0x1000
 */
#define _GNU_SOURCE
#include <upcie/upcie.h>
#include <libdrm/amdgpu.h>
#include <libdrm/amdgpu_drm.h>

int
main(int argc, char *argv[])
{
	struct iommufd iommufd = {0};
	struct iommufd_device dev = {0};
	uint32_t ioas_id = 0;
	uint32_t bar_index;
	uint64_t offset, length;
	const char *render_node = "/dev/dri/renderD128";
	int dbuf_fd = -1;
	int drm_fd = -1;
	int err;

	if (argc < 5 || argc > 6) {
		fprintf(stderr,
			"usage: %s <nvme_cdev> <bar_index> <offset> <length> [render_node]\n",
			argv[0]);
		return 2;
	}
	bar_index = (uint32_t)strtoul(argv[2], NULL, 0);
	offset = (uint64_t)strtoull(argv[3], NULL, 0);
	length = (uint64_t)strtoull(argv[4], NULL, 0);
	if (argc == 6) {
		render_node = argv[5];
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
		fprintf(stderr, "FAIL: vfio_device_bar_export_dmabuf err(%d)\n", dbuf_fd);
		err = dbuf_fd;
		goto out_detach;
	}
	printf("bar dma-buf: fd=%d region=%u offset=0x%" PRIx64 " length=0x%" PRIx64 "\n", dbuf_fd,
	       bar_index, offset, length);

	drm_fd = open(render_node, O_RDWR | O_CLOEXEC);
	if (drm_fd < 0) {
		fprintf(stderr, "FAIL: open('%s') errno(%d): %s\n", render_node, errno,
			strerror(errno));
		close(dbuf_fd);
		err = -errno;
		goto out_detach;
	}
	printf("drm render node: %s (fd=%d)\n", render_node, drm_fd);

	{
		amdgpu_device_handle amd_dev = NULL;
		uint32_t major = 0, minor = 0;
		struct amdgpu_bo_import_result result = {0};

		err = amdgpu_device_initialize(drm_fd, &major, &minor, &amd_dev);
		if (err) {
			fprintf(stderr, "FAIL: amdgpu_device_initialize err(%d): %s\n", err,
				strerror(-err));
			close(drm_fd);
			close(dbuf_fd);
			goto out_detach;
		}
		printf("amdgpu device: v%u.%u\n", major, minor);

		err = amdgpu_bo_import(amd_dev, amdgpu_bo_handle_type_dma_buf_fd,
				       (uint32_t)dbuf_fd, &result);
		if (err) {
			fprintf(stderr,
				"amdgpu_bo_import: FAIL err(%d): %s\n"
				"       amdgpu's dma-buf importer refused the vfio-pci\n"
				"       export at the DRM level (below HIP). This is the\n"
				"       generic amdgpu gate, not a HIP-specific policy.\n",
				err, strerror(-err));
			amdgpu_device_deinitialize(amd_dev);
			close(drm_fd);
			close(dbuf_fd);
			goto out_detach;
		}
		printf("amdgpu_bo_import: OK (alloc_size=0x%" PRIx64 ")\n", result.alloc_size);

		/* Step 3: try to map the imported BO into a GPU VA. This is the
		 * "does the amdgpu VM accept peer MMIO as a mapping target"
		 * question. If it succeeds, the GPU could DMA against that VA
		 * from a shader/compute kernel, and the only remaining gap
		 * (userspace) is a way to hand this VA to HIP.
		 */
		{
			uint64_t va_base = 0;
			amdgpu_va_handle va_range = NULL;
			int va_err;

			va_err = amdgpu_va_range_alloc(amd_dev, amdgpu_gpu_va_range_general,
						       result.alloc_size, 4096, 0, &va_base,
						       &va_range, 0);
			if (va_err) {
				fprintf(stderr,
					"amdgpu_va_range_alloc: FAIL err(%d): %s\n",
					va_err, strerror(-va_err));
			} else {
				printf("amdgpu_va_range_alloc: OK va_base=0x%" PRIx64 "\n",
				       va_base);

				va_err = amdgpu_bo_va_op(result.buf_handle, 0,
							 result.alloc_size, va_base, 0,
							 AMDGPU_VA_OP_MAP);
				if (va_err) {
					fprintf(stderr,
						"amdgpu_bo_va_op(MAP): FAIL err(%d): %s\n"
						"       The amdgpu VM refused to map the\n"
						"       peer-MMIO BO. GPU compute kernels\n"
						"       cannot address the NVMe BAR through\n"
						"       this path; the kernel helper the\n"
						"       GINA notebook prescribes is the\n"
						"       last-mile fix.\n",
						va_err, strerror(-va_err));
				} else {
					printf("amdgpu_bo_va_op(MAP): OK gpu_va=0x%" PRIx64
					       "\n",
					       va_base);
					printf("OK: NVMe BAR mapped into amdgpu GPU VM at"
					       " 0x%" PRIx64 "\n",
					       va_base);
					amdgpu_bo_va_op(result.buf_handle, 0,
							result.alloc_size, va_base, 0,
							AMDGPU_VA_OP_UNMAP);
				}
				amdgpu_va_range_free(va_range);
			}
		}

		if (result.buf_handle) {
			amdgpu_bo_free(result.buf_handle);
		}
		amdgpu_device_deinitialize(amd_dev);
	}
	close(drm_fd);
	close(dbuf_fd);

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
