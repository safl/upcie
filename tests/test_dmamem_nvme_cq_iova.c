// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * dmamem NVMe I/O qpair with the completion queue elsewhere
 * =========================================================
 *
 * Exercises nvme_controller_create_io_qpair_dmamem_cq_iova(): the I/O
 * completion queue is created at a caller-supplied IOVA rather than at the
 * dmamem CQ the qpair owns. Here the IOVA is a second heap buffer, so the
 * test needs nothing beyond what the IDENTIFY smoketest needs, but the
 * mechanism is the one that puts the CQ in device memory.
 *
 * For each of a few reads, checks that the completion lands in the caller's
 * buffer and not in qp->cq, that nvme_qpair_reap_cpl() sees nothing until the
 * entry is copied into qp->cq, and that it reaps the copy as usual.
 *
 * Takes the vfio cdev path as argv[1] or NVME_DMAMEM_CDEV, like the IDENTIFY
 * smoketest. Reads one block of namespace 1.
 */
#define _UPCIE_WITH_NVME
#include <upcie/upcie.h>

#define QDEPTH 32
#define NROUNDS 3
#define NVME_NVM_READ 0x02
#define NSID 1

static int
wait_for_cpl(volatile struct nvme_completion *cqe, uint16_t phase, int timeout_ms)
{
	for (int i = 0; i < timeout_ms; ++i) {
		if ((cqe->status & 0x1) == phase) {
			return 0;
		}
		usleep(1000);
	}

	return -EAGAIN;
}

static int
run_reads(struct nvme_controller *ctrlr, struct nvme_qpair *ioq, struct dmamem_heap *heap,
	  size_t cq_alt_offset)
{
	struct nvme_completion *cq_alt = dmamem_heap_at_va(heap, cq_alt_offset);
	struct nvme_completion *cq = ioq->cq;
	size_t buf_offset = 0;
	int err;

	err = dmamem_heap_alloc_aligned(heap, 4096, 4096, &buf_offset);
	if (err) {
		fprintf(stderr, "FAIL: dmamem_heap_alloc_aligned(buf) err(%d)\n", err);
		return err;
	}

	for (int round = 0; round < NROUNDS; ++round) {
		struct nvme_command cmd = {0};
		struct nvme_completion cpl = {0};

		cmd.opc = NVME_NVM_READ;
		cmd.cid = round + 1;
		cmd.nsid = NSID;
		cmd.prp1 = dmamem_heap_at_iova(heap, buf_offset);

		err = nvme_qpair_enqueue(ioq, &cmd);
		if (err) {
			fprintf(stderr, "FAIL: nvme_qpair_enqueue() err(%d)\n", err);
			goto out_free;
		}
		nvme_qpair_sqdb_update(ioq);

		err = wait_for_cpl(&cq_alt[round], 1, ctrlr->timeout_ms);
		if (err) {
			fprintf(stderr, "FAIL: no completion at cq_iova[%d]\n", round);
			goto out_free;
		}
		if (cq[round].status & 0x1) {
			fprintf(stderr, "FAIL: completion landed in qp->cq[%d]\n", round);
			err = -EIO;
			goto out_free;
		}

		err = nvme_qpair_reap_cpl(ioq, 1, &cpl);
		if (err != -EAGAIN) {
			fprintf(stderr,
				"FAIL: nvme_qpair_reap_cpl() saw a completion before the copy, "
				"err(%d)\n",
				err);
			err = err ? err : -EIO;
			goto out_free;
		}

		cq[round] = cq_alt[round];

		err = nvme_qpair_reap_cpl(ioq, ctrlr->timeout_ms, &cpl);
		if (err) {
			fprintf(stderr, "FAIL: nvme_qpair_reap_cpl() err(%d)\n", err);
			goto out_free;
		}
		if (cpl.cid != round + 1) {
			fprintf(stderr, "FAIL: cid(%u) expected(%d)\n", cpl.cid, round + 1);
			err = -EIO;
			goto out_free;
		}
		if ((cpl.status >> 1) & 0x7FF) {
			fprintf(stderr, "FAIL: READ CQE status(0x%x) sc(0x%x) sct(0x%x)\n",
				cpl.status, (cpl.status >> 1) & 0xFF, (cpl.status >> 9) & 0x7);
			err = -EIO;
			goto out_free;
		}
	}

	printf("OK: %d completions arrived at cq_iova, none in qp->cq\n", NROUNDS);

out_free:
	dmamem_heap_free(heap, buf_offset);
	return err;
}

int
main(int argc, char *argv[])
{
	struct iommufd iommufd = {0};
	struct dmamem dmem = {0};
	struct dmamem_heap heap = {0};
	struct nvme_controller ctrlr = {0};
	struct nvme_dmamem_vfio_ctx ctx = {0};
	struct nvme_qpair ioq = {0};
	size_t hugepgsz = 2ULL * 1024 * 1024;
	size_t nhugepages = 8;
	size_t sq_offset = 0, cq_offset = 0, prp_offset = 0, cq_alt_offset = 0;
	const char *cdev_path;
	int err;

	cdev_path = (argc > 1) ? argv[1] : getenv("NVME_DMAMEM_CDEV");
	if (!cdev_path) {
		fprintf(stderr,
			"usage: %s <vfio-cdev-path>\n"
			"   or  NVME_DMAMEM_CDEV=/dev/vfio/devices/vfioN %s\n",
			argv[0], argv[0]);
		return 2;
	}

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

	err = dmamem_from_memfd(&dmem, &iommufd, hugepgsz * nhugepages, hugepgsz);
	if (err) {
		fprintf(stderr, "FAIL: dmamem_from_memfd() err(%d): %s\n", err, strerror(-err));
		goto out_ioas;
	}

	err = dmamem_heap_init(&heap, &dmem, 4096);
	if (err) {
		fprintf(stderr, "FAIL: dmamem_heap_init() err(%d)\n", err);
		goto out_dmamem;
	}

	err = nvme_controller_open_dmamem_vfio(&ctrlr, &ctx, &iommufd, &heap, cdev_path);
	if (err) {
		fprintf(stderr, "FAIL: nvme_controller_open_dmamem_vfio() err(%d): %s\n", err,
			strerror(-err));
		goto out_heap;
	}

	err = dmamem_heap_alloc_aligned(&heap, 4096, 4096, &cq_alt_offset);
	if (err) {
		fprintf(stderr, "FAIL: dmamem_heap_alloc_aligned(cq_alt) err(%d)\n", err);
		goto out_close;
	}
	memset(dmamem_heap_at_va(&heap, cq_alt_offset), 0, 4096);

	err = nvme_controller_create_io_qpair_dmamem_cq_iova(
		&ctrlr, &ioq, QDEPTH, &heap, &sq_offset, &cq_offset, &prp_offset,
		dmamem_heap_at_iova(&heap, cq_alt_offset));
	if (err) {
		fprintf(stderr, "FAIL: nvme_controller_create_io_qpair_dmamem_cq_iova() err(%d)\n",
			err);
		goto out_cq_alt;
	}

	err = run_reads(&ctrlr, &ioq, &heap, cq_alt_offset);

	if (nvme_controller_delete_io_qpair_dmamem(&ctrlr, &ioq, &heap, sq_offset, cq_offset,
						   prp_offset)) {
		fprintf(stderr, "FAIL: nvme_controller_delete_io_qpair_dmamem()\n");
		err = err ? err : -EIO;
	}

out_cq_alt:
	dmamem_heap_free(&heap, cq_alt_offset);
out_close:
	nvme_controller_close_dmamem_vfio(&ctrlr, &ctx, &heap);
out_heap:
	dmamem_heap_term(&heap);
out_dmamem:
	dmamem_destroy(&dmem);
out_ioas:
	iommufd_destroy(&iommufd, iommufd.ioas_id);
out_iommufd:
	iommufd_close(&iommufd);
	return err ? 1 : 0;
}
