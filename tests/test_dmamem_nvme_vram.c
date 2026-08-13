// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * NVMe DMA into GPU VRAM via a shared iommufd IOAS (end-to-end proof)
 * ==================================================================
 *
 * Attach an NVMe controller and a vfio-pci-bound GPU to the SAME iommufd
 * IOAS, import the GPU's BAR (the VRAM window when ReBAR is on) as a
 * dmamem via VFIO_DEVICE_FEATURE_DMA_BUF, and issue an NVMe IDENTIFY
 * CONTROLLER whose PRP1 points into that VRAM IOVA range. The controller
 * DMAs the 4 KiB identify response directly into GPU memory; we then peek
 * at that VRAM offset by mmap-ing the GPU BAR through the vfio device fd
 * and checking that the controller SN prefix landed at the expected byte
 * position of the identify structure.
 *
 * On top of that IDENTIFY proof, an IO-queue-pair WRITE exercises the
 * fragile peer direction: the controller *reads* GPU VRAM over PCIe
 * (PRP1 in VRAM) and writes it to LBA 0, verified by reading LBA 0 back
 * into host memory. This overwrites LBA 0 on the DUT.
 *
 * Usage:
 *   test_dmamem_nvme_vram <nvme_cdev> <gpu_cdev> <gpu_bar_index> <gpu_bar_size>
 *   e.g.
 *   test_dmamem_nvme_vram /dev/vfio/devices/vfio0 /dev/vfio/devices/vfio1 0 0x400000000
 */
#define _UPCIE_WITH_NVME
#include <upcie/upcie.h>

#define IDENTIFY_OFFSET 0x2000 /* 8 KiB into VRAM, arbitrary */

static int
open_gpu_and_import_bar(struct iommufd *iommufd, const char *gpu_cdev, uint32_t bar_index,
			uint64_t bar_size, struct vfio_cdev *dev_out, struct dmamem *bar_dmem_out,
			void **bar_va_out, size_t *bar_va_size_out)
{
	struct vfio_region_info region = {0};
	int dbuf_fd;
	int err;

	err = vfio_cdev_open(gpu_cdev, dev_out);
	if (err) {
		fprintf(stderr, "FAIL: vfio_cdev_open(gpu, '%s') err(%d)\n", gpu_cdev, err);
		return err;
	}

	err = iommufd_bind(iommufd, dev_out);
	if (err) {
		fprintf(stderr, "FAIL: iommufd_bind(gpu) err(%d)\n", err);
		goto err_close;
	}

	err = iommufd_attach(iommufd, dev_out);
	if (err) {
		fprintf(stderr, "FAIL: iommufd_attach(gpu) err(%d)\n", err);
		goto err_close;
	}

	dbuf_fd = vfio_device_bar_export_dmabuf(dev_out->fd, bar_index, 0, bar_size);
	if (dbuf_fd < 0) {
		fprintf(stderr, "FAIL: vfio_device_bar_export_dmabuf(region=%u) err(%d)\n",
			bar_index, dbuf_fd);
		err = dbuf_fd;
		goto err_detach;
	}

	err = dmamem_from_dmabuf(bar_dmem_out, iommufd, dbuf_fd, bar_size);
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
	iommufd_detach(dev_out);
err_close:
	vfio_cdev_close(dev_out);
	return err;
}

int
main(int argc, char *argv[])
{
	struct iommufd iommufd = {0};
	struct vfio_cdev gpu_dev = {0};
	struct dmamem gpu_bar_dmem = {0};
	struct dmamem admin_dmem = {0};
	struct dmamem_heap admin_heap = {0};
	struct nvme_controller ctrlr = {0};
	struct nvme_dmamem_vfio_ctx nvme_ctx = {0};
	struct nvme_command cmd = {0};
	struct nvme_completion cpl = {0};
	void *gpu_bar_va = NULL;
	size_t gpu_bar_va_size = 0;
	uint32_t gpu_bar_index;
	uint64_t gpu_bar_size;
	uint64_t identify_iova;
	int err;

	if (argc != 5) {
		fprintf(stderr,
			"usage: %s <nvme_cdev> <gpu_cdev> <gpu_bar_index> <gpu_bar_size>\n"
			"  e.g. %s /dev/vfio/devices/vfio0 /dev/vfio/devices/vfio1 0 0x400000000\n",
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
	err = iommufd_ioas_alloc(&iommufd);
	if (err) {
		fprintf(stderr, "FAIL: iommufd_ioas_alloc err(%d)\n", err);
		goto out_iommufd;
	}

	err = open_gpu_and_import_bar(&iommufd, argv[2], gpu_bar_index, gpu_bar_size, &gpu_dev,
				      &gpu_bar_dmem, &gpu_bar_va, &gpu_bar_va_size);
	if (err) {
		goto out_ioas;
	}
	printf("gpu BAR imported: base_iova=0x%" PRIx64 " size=0x%" PRIx64
	       ", cpu bar view=%p size=%zu\n",
	       gpu_bar_dmem.base_iova, (uint64_t)gpu_bar_dmem.size, gpu_bar_va, gpu_bar_va_size);

	/* Hugepage-backed dmamem for the NVMe admin queue backing; the per-request
	 * PRP scratch alone is NVME_REQUEST_POOL_LEN pages. */
	err = dmamem_from_memfd(&admin_dmem, &iommufd, 16ULL * 1024 * 1024, 2ULL * 1024 * 1024);
	if (err) {
		fprintf(stderr, "FAIL: dmamem_from_memfd(admin) err(%d)\n", err);
		goto out_gpu;
	}
	err = dmamem_heap_init(&admin_heap, &admin_dmem, 4096);
	if (err) {
		fprintf(stderr, "FAIL: dmamem_heap_init(admin) err(%d)\n", err);
		goto out_admin_dmem;
	}

	err = nvme_controller_open_dmamem_vfio(&ctrlr, &nvme_ctx, &iommufd, &admin_heap, argv[1]);
	if (err) {
		fprintf(stderr, "FAIL: nvme_controller_open_dmamem_vfio err(%d)\n", err);
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

	printf("issue: IDENTIFY CONTROLLER, PRP1=0x%" PRIx64 " (gpu VRAM IOVA base 0x%" PRIx64
	       " + offset 0x%x)\n",
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

	/*
	 * WRITE from VRAM through an IO queue pair: the controller must *read*
	 * GPU memory over PCIe (PRP1 in VRAM, data flowing device-inbound), the
	 * more fragile peer direction. Create an IO qpair on the dmamem path,
	 * prime a VRAM offset with a known pattern, WRITE it to LBA 0, then read
	 * LBA 0 back into host memory and confirm the pattern round-tripped.
	 * This overwrites LBA 0 on the DUT.
	 */
	{
		struct nvme_qpair ioq = {0};
		size_t ioq_sq = 0, ioq_cq = 0, ioq_prp = 0;
		const uint32_t NSID = 1;
		const uint32_t NBLOCKS_M1 = 7; /* 8 blocks (0-based) = 4 KiB */
		const size_t WRITE_OFFSET = 0x4000;
		uint8_t *vram = (uint8_t *)gpu_bar_va + WRITE_OFFSET;
		uint64_t write_iova = gpu_bar_dmem.base_iova + WRITE_OFFSET;
		size_t verify_off = 0;
		uint8_t *verify_va;

		err = nvme_controller_create_io_qpair_dmamem(&ctrlr, &ioq, 32, &admin_heap,
							     &ioq_sq, &ioq_cq, &ioq_prp);
		if (err) {
			fprintf(stderr, "FAIL: nvme_controller_create_io_qpair_dmamem err(%d)\n",
				err);
			goto out_ctrlr;
		}

		err = dmamem_heap_alloc_aligned(&admin_heap, 4096, 4096, &verify_off);
		if (err) {
			fprintf(stderr, "FAIL: dmamem_heap_alloc_aligned(verify buf) err(%d)\n",
				err);
			goto io_done;
		}
		verify_va = dmamem_heap_at_va(&admin_heap, verify_off);

		/* Prime the VRAM source with a recognizable pattern. */
		for (size_t i = 0; i < 4096; i++) {
			vram[i] = (uint8_t)(0xA5 ^ (i & 0xFF));
		}

		/* WRITE LBA 0 from VRAM: the controller reads GPU memory. */
		memset(&cmd, 0, sizeof(cmd));
		cmd.opc = 0x01; /* NVMe WRITE */
		cmd.cid = 1;
		cmd.nsid = NSID;
		cmd.prp1 = write_iova;
		cmd.cdw12 = NBLOCKS_M1;
		printf("issue: NVMe WRITE LBA 0..%u <- PRP1=0x%" PRIx64
		       " (VRAM IOVA base 0x%" PRIx64 " + offset 0x%zx)\n",
		       NBLOCKS_M1 + 1, write_iova, gpu_bar_dmem.base_iova, WRITE_OFFSET);
		err = nvme_qpair_submit_sync(&ioq, &cmd, ctrlr.timeout_ms, &cpl);
		if (err) {
			fprintf(stderr, "FAIL: WRITE from VRAM err(%d)\n", err);
			goto io_done;
		}

		/* Read LBA 0 back to host memory and confirm the pattern. */
		memset(&cmd, 0, sizeof(cmd));
		cmd.opc = 0x02; /* NVMe READ */
		cmd.cid = 1;
		cmd.nsid = NSID;
		cmd.prp1 = dmamem_heap_at_iova(&admin_heap, verify_off);
		cmd.cdw12 = NBLOCKS_M1;
		err = nvme_qpair_submit_sync(&ioq, &cmd, ctrlr.timeout_ms, &cpl);
		if (err) {
			fprintf(stderr, "FAIL: verify READ LBA 0 err(%d)\n", err);
		} else if (memcmp(verify_va, vram, 4096) == 0) {
			printf("OK: NVMe WRITE from VRAM round-tripped through LBA 0"
			       " (device read GPU memory over PCIe)\n");
		} else {
			fprintf(stderr, "FAIL: LBA 0 readback does not match the VRAM pattern\n");
			err = -EIO;
		}

io_done:
		{
			int derr = nvme_controller_delete_io_qpair_dmamem(
				&ctrlr, &ioq, &admin_heap, ioq_sq, ioq_cq, ioq_prp);
			if (derr && !err) {
				err = derr;
			}
		}
	}

out_ctrlr:
	nvme_controller_close_dmamem_vfio(&ctrlr, &nvme_ctx, &admin_heap);
out_admin_heap:
	dmamem_heap_term(&admin_heap);
out_admin_dmem:
	dmamem_destroy(&admin_dmem);
out_gpu:
	if (gpu_bar_va) {
		munmap(gpu_bar_va, gpu_bar_va_size);
	}
	dmamem_destroy(&gpu_bar_dmem);
	iommufd_detach(&gpu_dev);
	vfio_cdev_close(&gpu_dev);
out_ioas:
	iommufd_destroy(&iommufd, iommufd.ioas_id);
out_iommufd:
	iommufd_close(&iommufd);
	return err ? 1 : 0;
}
