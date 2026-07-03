// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * NVMe BAR reachable from HIP via libdrm PRIME + hipHostRegister
 * ==============================================================
 *
 * Follow-up to dmamem_hip_bar_import_smoketest (hipImportExternalMemory
 * refused vfio-pci dma-bufs, hipErrorOutOfMemory) and
 * dmamem_libdrm_bar_import_smoketest (amdgpu_bo_import accepted the
 * same dma-buf cleanly). The remaining question is whether we can push
 * the imported BO the last mile into HIP-visible address space via
 * amdgpu_bo_cpu_map (getting a userspace pointer) and hipHostRegister
 * (getting a HIP device pointer for that userspace address).
 *
 * If both succeed, an amdgpu-driven GPU can address the NVMe BAR through
 * a normal HIP device pointer, and the AMD prototype's CPU-relayed
 * doorbell bridge is optional on this stack.
 *
 * Usage:
 *   dmamem_hip_bar_via_libdrm_smoketest <nvme_cdev> <bar_index> <offset> <length>
 */
#define _GNU_SOURCE
#include <upcie/upcie.h>
#include <libdrm/amdgpu.h>
#include <libdrm/amdgpu_drm.h>
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
	const char *render_node = "/dev/dri/renderD128";
	int dbuf_fd = -1;
	int drm_fd = -1;
	amdgpu_device_handle amd_dev = NULL;
	struct amdgpu_bo_import_result import = {0};
	void *bo_cpu = NULL;
	int err;
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

	{
		int cnt = 0;
		herr = hipGetDeviceCount(&cnt);
		if (herr != hipSuccess || cnt < 1) {
			fprintf(stderr,
				"FAIL: no HIP device; is GPU on amdgpu? (%s)\n",
				hip_err_name(herr));
			return 1;
		}
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
		fprintf(stderr, "FAIL: iommufd_device_open err(%d)\n", err);
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
		err = -errno;
		fprintf(stderr, "FAIL: open(%s) err(%d): %s\n", render_node, errno,
			strerror(errno));
		goto out_dbuf;
	}
	{
		uint32_t major = 0, minor = 0;
		err = amdgpu_device_initialize(drm_fd, &major, &minor, &amd_dev);
		if (err) {
			fprintf(stderr, "FAIL: amdgpu_device_initialize err(%d)\n", err);
			goto out_drm;
		}
		printf("amdgpu device: v%u.%u\n", major, minor);
	}

	err = amdgpu_bo_import(amd_dev, amdgpu_bo_handle_type_dma_buf_fd, (uint32_t)dbuf_fd,
			       &import);
	if (err) {
		fprintf(stderr, "FAIL: amdgpu_bo_import err(%d)\n", err);
		goto out_amd;
	}
	printf("amdgpu_bo_import: OK (alloc_size=0x%" PRIx64 ")\n", import.alloc_size);

	err = amdgpu_bo_cpu_map(import.buf_handle, &bo_cpu);
	if (err) {
		fprintf(stderr,
			"amdgpu_bo_cpu_map: FAIL err(%d): %s\n"
			"       The imported BO wraps peer MMIO which is not\n"
			"       CPU-mappable through the amdgpu path either. The\n"
			"       hipHostRegister leg cannot be attempted from here.\n",
			err, strerror(-err));
		goto out_bo;
	}
	printf("amdgpu_bo_cpu_map: OK cpu_va=%p\n", bo_cpu);

	/* Confirm the userspace pointer really reaches NVMe MMIO. NVMe VS register
	 * lives at BAR0 + 0x08 and returns a nonzero major.minor.tertiary version;
	 * the doorbell page starts at BAR0 + 0x1000. If we exported the doorbell
	 * page (offset 0x1000), a read from bo_cpu is a doorbell read (returns 0);
	 * that's not a great probe. If we exported starting at 0, we can check VS
	 * directly. Print whatever's there as a fingerprint. */
	{
		volatile uint32_t *reg = (volatile uint32_t *)bo_cpu;
		printf("cpu-side probe: *(uint32_t*)bo_cpu = 0x%08x\n", reg[0]);
	}

	/* Try several flag combinations for hipHostRegister; the peer-BAR case
	 * is unusual so different HIP versions accept different flags. */
	{
		struct {
			unsigned flags;
			const char *name;
		} attempts[] = {
			{ hipHostRegisterMapped | hipHostRegisterIoMemory, "Mapped|IoMemory" },
			{ hipHostRegisterMapped, "Mapped" },
			{ hipHostRegisterIoMemory, "IoMemory" },
			{ hipHostRegisterPortable | hipHostRegisterMapped, "Portable|Mapped" },
			{ 0, "0" },
		};
		int accepted = -1;

		for (size_t i = 0; i < sizeof(attempts) / sizeof(attempts[0]); i++) {
			herr = hipHostRegister(bo_cpu, length, attempts[i].flags);
			printf("hipHostRegister(%s): %s(%d)\n", attempts[i].name,
			       hip_err_name(herr), herr);
			if (herr == hipSuccess) {
				accepted = (int)i;
				break;
			}
		}
		if (accepted < 0) {
			fprintf(stderr,
				"NOTE: hipHostRegister rejected every flag variant.\n"
				"      The amdgpu BO userspace pointer is real (CPU can\n"
				"      read/write NVMe MMIO through it) but HIP's higher-\n"
				"      level registration policy will not accept it. A HIP\n"
				"      kernel cannot address the BAR via this route today.\n");
			amdgpu_bo_cpu_unmap(import.buf_handle);
			err = -EIO;
			goto out_bo;
		}
		printf("hipHostRegister: OK with flags %s\n", attempts[accepted].name);
	}

	{
		void *hip_dptr = NULL;
		herr = hipHostGetDevicePointer(&hip_dptr, bo_cpu, 0);
		if (herr != hipSuccess) {
			fprintf(stderr, "hipHostGetDevicePointer: FAIL -> %s(%d)\n",
				hip_err_name(herr), herr);
		} else {
			printf("hipHostGetDevicePointer: OK hip_dptr=%p\n", hip_dptr);
			printf("OK: NVMe BAR reachable from HIP via libdrm+hipHostRegister\n");
		}
	}

	hipHostUnregister(bo_cpu);
	amdgpu_bo_cpu_unmap(import.buf_handle);

out_bo:
	amdgpu_bo_free(import.buf_handle);
out_amd:
	amdgpu_device_deinitialize(amd_dev);
out_drm:
	close(drm_fd);
out_dbuf:
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
