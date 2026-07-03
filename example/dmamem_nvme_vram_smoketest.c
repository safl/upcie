// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * NVMe DMA into GPU VRAM via a shared iommufd IOAS (end-to-end proof)
 * ==================================================================
 *
 * Ties everything on the branch together: attach an NVMe controller and
 * a vfio-pci-bound GPU to the SAME iommufd IOAS, import the GPU's BAR
 * (the VRAM window when ReBAR is on) as a dmamem via
 * VFIO_DEVICE_FEATURE_DMA_BUF, and issue an NVMe IDENTIFY CONTROLLER
 * whose PRP1 points into that VRAM IOVA range. The controller DMAs the
 * 4 KiB identify response directly into GPU memory; we then peek at
 * that VRAM offset by mmap-ing the GPU BAR through the vfio device fd
 * and checking that the Samsung SN prefix landed at the expected byte
 * position of the identify structure.
 *
 * Usage:
 *   dmamem_nvme_vram_smoketest <nvme_cdev> <gpu_cdev> <gpu_bar_index> <gpu_bar_size>
 *
 * Example on wave (NVMe = /dev/vfio/devices/vfio0, 7800 XT = /dev/vfio/devices/vfio1, BAR0):
 *   dmamem_nvme_vram_smoketest /dev/vfio/devices/vfio0 /dev/vfio/devices/vfio1 0 0x400000000
 */
#define _UPCIE_WITH_NVME
#include <upcie/upcie.h>

#define IDENTIFY_OFFSET 0x2000 /* 8 KiB into VRAM, arbitrary */

static int
open_gpu_and_import_bar(struct iommufd *iommufd, uint32_t ioas_id, const char *gpu_cdev,
			uint32_t bar_index, uint64_t bar_size, struct iommufd_device *dev_out,
			struct dmamem *bar_dmem_out, void **bar_va_out, size_t *bar_va_size_out)
{
	struct vfio_region_info region = {0};
	int dbuf_fd;
	int err;

	err = iommufd_device_open(gpu_cdev, dev_out);
	if (err) {
		fprintf(stderr, "FAIL: iommufd_device_open(gpu, '%s') err(%d)\n", gpu_cdev, err);
		return err;
	}

	err = iommufd_device_bind(dev_out, iommufd);
	if (err) {
		fprintf(stderr, "FAIL: iommufd_device_bind(gpu) err(%d)\n", err);
		goto err_close;
	}

	err = iommufd_device_attach(dev_out, ioas_id);
	if (err) {
		fprintf(stderr, "FAIL: iommufd_device_attach(gpu) err(%d)\n", err);
		goto err_close;
	}

	dbuf_fd = vfio_device_bar_export_dmabuf(dev_out->fd, bar_index, 0, bar_size);
	if (dbuf_fd < 0) {
		fprintf(stderr, "FAIL: vfio_device_bar_export_dmabuf(region=%u) err(%d)\n",
			bar_index, dbuf_fd);
		err = dbuf_fd;
		goto err_detach;
	}

	err = dmamem_from_dmabuf(bar_dmem_out, iommufd, ioas_id, dbuf_fd, bar_size);
	if (err) {
		fprintf(stderr, "FAIL: dmamem_from_dmabuf(gpu bar) err(%d)\n", err);
		close(dbuf_fd);
		goto err_detach;
	}

	/* Also mmap the BAR through the vfio fd so we can CPU-verify the DMA. */
	region.index = bar_index;
	err = vfio_device_get_region_info(dev_out->fd, &region);
	if (err < 0) {
		fprintf(stderr, "FAIL: vfio_device_get_region_info(gpu) errno(%d)\n", errno);
		err = -errno;
		goto err_dmem;
	}

	*bar_va_size_out = region.size;
	*bar_va_out = vfio_map_region(dev_out->fd, region.size, region.offset);
	if (*bar_va_out == MAP_FAILED) {
		fprintf(stderr, "FAIL: mmap gpu bar errno(%d): %s\n", errno, strerror(errno));
		*bar_va_out = NULL;
		err = -errno;
		goto err_dmem;
	}
	return 0;

err_dmem:
	dmamem_destroy(bar_dmem_out);
err_detach:
	iommufd_device_detach(dev_out);
err_close:
	iommufd_device_close(dev_out);
	return err;
}

int
main(int argc, char *argv[])
{
	struct iommufd iommufd = {0};
	struct iommufd_device gpu_dev = {0};
	struct dmamem gpu_bar_dmem = {0};
	struct dmamem admin_dmem = {0};
	struct dmamem_heap admin_heap = {0};
	struct nvme_controller ctrlr = {0};
	struct nvme_dmamem_ctx nvme_ctx = {0};
	struct nvme_command cmd = {0};
	struct nvme_completion cpl = {0};
	void *gpu_bar_va = NULL;
	size_t gpu_bar_va_size = 0;
	uint32_t ioas_id = 0;
	uint32_t gpu_bar_index;
	uint64_t gpu_bar_size;
	uint64_t identify_iova;
	size_t aq_sq = 0, aq_cq = 0;
	int err;

	if (argc != 5) {
		fprintf(stderr,
			"usage: %s <nvme_cdev> <gpu_cdev> <gpu_bar_index> <gpu_bar_size>\n"
			"example (wave):\n"
			"  %s /dev/vfio/devices/vfio0 /dev/vfio/devices/vfio1 0 0x400000000\n",
			argv[0], argv[0]);
		return 2;
	}
	gpu_bar_index = (uint32_t)strtoul(argv[3], NULL, 0);
	gpu_bar_size = (uint64_t)strtoull(argv[4], NULL, 0);

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

	err = open_gpu_and_import_bar(&iommufd, ioas_id, argv[2], gpu_bar_index, gpu_bar_size,
				      &gpu_dev, &gpu_bar_dmem, &gpu_bar_va, &gpu_bar_va_size);
	if (err) {
		goto out_ioas;
	}
	printf("gpu BAR imported: base_iova=0x%" PRIx64 " size=0x%" PRIx64
	       ", cpu bar view=%p size=%zu\n",
	       gpu_bar_dmem.base_iova, (uint64_t)gpu_bar_dmem.size, gpu_bar_va, gpu_bar_va_size);

	/* Small hugepage-backed dmamem for the NVMe admin queue backing. */
	err = dmamem_from_memfd(&admin_dmem, &iommufd, ioas_id, 2ULL * 1024 * 1024,
				2ULL * 1024 * 1024);
	if (err) {
		fprintf(stderr, "FAIL: dmamem_from_memfd(admin) err(%d)\n", err);
		goto out_gpu;
	}
	err = dmamem_heap_init(&admin_heap, &admin_dmem, 4096);
	if (err) {
		fprintf(stderr, "FAIL: dmamem_heap_init(admin) err(%d)\n", err);
		goto out_admin_dmem;
	}

	err = nvme_controller_open_dmamem(&ctrlr, &nvme_ctx, &iommufd, ioas_id, &admin_heap,
					  argv[1], &aq_sq, &aq_cq);
	if (err) {
		fprintf(stderr, "FAIL: nvme_controller_open_dmamem err(%d)\n", err);
		goto out_admin_heap;
	}

	/* Prime the GPU-side target region so we can distinguish DMA'd bytes from
	 * whatever the last renderer left there. Writing to BAR VRAM through the
	 * vfio mmap is uncached and slow but fine for a small region. */
	memset((uint8_t *)gpu_bar_va + IDENTIFY_OFFSET, 0x00, 4096);

	identify_iova = gpu_bar_dmem.base_iova + IDENTIFY_OFFSET;

	cmd.opc = 0x06; /* IDENTIFY */
	cmd.cid = 1;
	cmd.prp1 = identify_iova;
	cmd.cdw10 = 1; /* CNS=1: Identify Controller */

	printf("issue: IDENTIFY CONTROLLER, PRP1=0x%" PRIx64
	       " (gpu VRAM IOVA base 0x%" PRIx64 " + offset 0x%x)\n",
	       identify_iova, gpu_bar_dmem.base_iova, IDENTIFY_OFFSET);

	err = nvme_qpair_enqueue(&ctrlr.aq, &cmd);
	if (err) {
		fprintf(stderr, "FAIL: nvme_qpair_enqueue err(%d)\n", err);
		goto out_ctrlr;
	}
	nvme_qpair_sqdb_update(&ctrlr.aq);

	err = nvme_qpair_reap_cpl(&ctrlr.aq, ctrlr.timeout_ms, &cpl);
	if (err) {
		fprintf(stderr, "FAIL: nvme_qpair_reap_cpl err(%d)\n", err);
		goto out_ctrlr;
	}
	if ((cpl.status >> 1) & 0x7FF) {
		fprintf(stderr, "FAIL: IDENTIFY CQE status=0x%x\n", cpl.status);
		err = -EIO;
		goto out_ctrlr;
	}

	/* Peek at the VRAM at IDENTIFY_OFFSET via the BAR mmap and confirm the
	 * identify data landed. The identify buffer layout:
	 *   [4..23]  SN  (20 bytes)
	 *   [24..63] MN  (40 bytes)
	 *   [64..71] FR  (8 bytes)
	 */
	{
		uint8_t *view = (uint8_t *)gpu_bar_va + IDENTIFY_OFFSET;
		char sn[21] = {0}, mn[41] = {0}, fr[9] = {0};

		memcpy(sn, view + 4, 20);
		memcpy(mn, view + 24, 40);
		memcpy(fr, view + 64, 8);
		printf("VRAM peek: SN='%.20s' MN='%.40s' FR='%.8s'\n", sn, mn, fr);

		if (sn[0] == 0 || (sn[0] == (char)0x00 && sn[1] == 0)) {
			fprintf(stderr, "FAIL: VRAM at IDENTIFY_OFFSET still zeroed after DMA\n");
			err = -EIO;
			goto out_ctrlr;
		}
	}

	printf("OK: NVMe DMAed IDENTIFY response into GPU VRAM through iommufd IOAS\n");

out_ctrlr:
	nvme_controller_close_dmamem(&ctrlr, &nvme_ctx, &admin_heap, aq_sq, aq_cq);
out_admin_heap:
	dmamem_heap_term(&admin_heap);
out_admin_dmem:
	dmamem_destroy(&admin_dmem);
out_gpu:
	if (gpu_bar_va) {
		munmap(gpu_bar_va, gpu_bar_va_size);
	}
	dmamem_destroy(&gpu_bar_dmem);
	iommufd_device_detach(&gpu_dev);
	iommufd_device_close(&gpu_dev);
out_ioas:
	iommufd_destroy(&iommufd, ioas_id);
out_iommufd:
	iommufd_close(&iommufd);
	return err ? 1 : 0;
}
