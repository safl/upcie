// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * dmamem NVMe I/O qpair as deep as the request pool allows
 * ========================================================
 *
 * Creates an I/O qpair of NVME_REQUEST_POOL_LEN + 1 entries, the deepest the
 * request pool can keep full, and reads through it one command at a time for
 * three times its depth, so the tail crosses the last entry twice. Each
 * completion must carry the identifier of the command just submitted and a
 * success status. A submission queue carved smaller than its depth puts its
 * last entry in the memory that follows, where the reaper takes the command's
 * bytes for a completion.
 *
 * Then checks that a qpair one entry deeper is refused with -ERANGE.
 *
 * Takes the vfio cdev path as argv[1] or NVME_DMAMEM_CDEV, like the IDENTIFY
 * smoketest. Reads one block of namespace 1.
 */
#define _UPCIE_WITH_NVME
#include <upcie/upcie.h>

#define QDEPTH (NVME_REQUEST_POOL_LEN + 1)
#define NREADS (3 * QDEPTH)
#define NVME_NVM_READ 0x02
#define NSID 1

static int
run_reads(struct nvme_controller *ctrlr, struct nvme_qpair *ioq, struct dmamem_heap *heap)
{
	size_t buf_offset = 0;
	int err;

	err = dmamem_heap_alloc_aligned(heap, 4096, 4096, &buf_offset);
	if (err) {
		fprintf(stderr, "FAIL: dmamem_heap_alloc_aligned(buf) err(%d)\n", err);
		return err;
	}

	for (int i = 0; i < NREADS; ++i) {
		struct nvme_command cmd = {0};
		struct nvme_completion cpl = {0};
		uint16_t cid = (i % NVME_REQUEST_POOL_LEN) + 1;

		cmd.opc = NVME_NVM_READ;
		cmd.cid = cid;
		cmd.nsid = NSID;
		cmd.prp1 = dmamem_heap_at_iova(heap, buf_offset);

		err = nvme_qpair_enqueue(ioq, &cmd);
		if (err) {
			fprintf(stderr, "FAIL: nvme_qpair_enqueue() err(%d)\n", err);
			goto out_free;
		}
		nvme_qpair_sqdb_update(ioq);

		err = nvme_qpair_reap_cpl(ioq, ctrlr->timeout_ms, &cpl);
		if (err) {
			fprintf(stderr, "FAIL: read(%d) nvme_qpair_reap_cpl() err(%d)\n", i, err);
			goto out_free;
		}
		if (cpl.cid != cid) {
			fprintf(stderr, "FAIL: read(%d) cid(%u) expected(%u)\n", i, cpl.cid, cid);
			err = -EIO;
			goto out_free;
		}
		if ((cpl.status >> 1) & 0x7FF) {
			fprintf(stderr, "FAIL: read(%d) status(0x%x) sc(0x%x) sct(0x%x)\n", i,
				cpl.status, (cpl.status >> 1) & 0xFF, (cpl.status >> 9) & 0x7);
			err = -EIO;
			goto out_free;
		}
	}

	printf("OK: %d reads through a qpair of depth %d\n", NREADS, QDEPTH);

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
	size_t sq_offset = 0, cq_offset = 0, prp_offset = 0;
	const char *cdev_path;
	uint32_t mqes;
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

	mqes = nvme_reg_cap_get_mqes(nvme_mmio_cap_read(ctrlr.func.bars[0].region));
	if (mqes < QDEPTH - 1) {
		printf("SKIP: CAP.MQES(%u) is below depth(%d) - 1\n", mqes, QDEPTH);
		err = 0;
		goto out_close;
	}

	err = nvme_controller_create_io_qpair_dmamem(&ctrlr, &ioq, QDEPTH + 1, &heap, &sq_offset,
						     &cq_offset, &prp_offset);
	if (err != -ERANGE) {
		fprintf(stderr,
			"FAIL: depth(%d) past the request pool gave err(%d), not -ERANGE\n",
			QDEPTH + 1, err);
		if (!err) {
			nvme_controller_delete_io_qpair_dmamem(&ctrlr, &ioq, &heap, sq_offset,
							       cq_offset, prp_offset);
		}
		err = -EIO;
		goto out_close;
	}

	err = nvme_controller_create_io_qpair_dmamem(&ctrlr, &ioq, QDEPTH, &heap, &sq_offset,
						     &cq_offset, &prp_offset);
	if (err) {
		fprintf(stderr, "FAIL: nvme_controller_create_io_qpair_dmamem() err(%d)\n", err);
		goto out_close;
	}

	err = run_reads(&ctrlr, &ioq, &heap);

	if (nvme_controller_delete_io_qpair_dmamem(&ctrlr, &ioq, &heap, sq_offset, cq_offset,
						   prp_offset)) {
		fprintf(stderr, "FAIL: nvme_controller_delete_io_qpair_dmamem()\n");
		err = err ? err : -EIO;
	}

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
