// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * dmamem NVMe I/O qpairs up to the controller's allocation
 * ========================================================
 *
 * Opens the controller, which sends Set Features Number of Queues, and checks
 * that the counts kept on the controller are what Get Features Number of
 * Queues reports. Then creates I/O qpairs until the controller has none left:
 * exactly min(nsqa, ncqa) are created, and the next one fails with -ENOMEM.
 *
 * Takes the vfio cdev path as argv[1] or NVME_DMAMEM_CDEV.
 */
#define _UPCIE_WITH_NVME
#include <upcie/upcie.h>

#define DEPTH 2
#define NQPAIRS_MAX 128

struct qpair_held {
	struct nvme_qpair qp;
	size_t sq_offset, cq_offset, prp_offset;
};

static int
gfeat_nqueues(struct nvme_controller *ctrlr, uint32_t *nsqa, uint32_t *ncqa)
{
	struct nvme_command cmd = {0};
	struct nvme_completion cpl = {0};
	int err;

	cmd.opc = 0xA;   ///< Get Features
	cmd.cdw10 = 0x7; ///< Number of Queues, current value

	err = nvme_qpair_submit_sync(&ctrlr->aq, &cmd, ctrlr->base.admin_timeout_ms, &cpl);
	if (err) {
		return err;
	}

	*nsqa = (cpl.cdw0 & 0xFFFF) + 1;
	*ncqa = (cpl.cdw0 >> 16) + 1;

	return 0;
}

static int
run(struct nvme_controller *ctrlr, struct dmamem_heap *heap)
{
	static struct qpair_held held[NQPAIRS_MAX + 1];
	uint32_t nsqa = 0, ncqa = 0, nqpairs, created = 0;
	int err;

	err = gfeat_nqueues(ctrlr, &nsqa, &ncqa);
	if (err) {
		fprintf(stderr, "FAIL: Get Features Number of Queues err(%d)\n", err);
		return err;
	}
	printf("INFO: nsqa(%u) ncqa(%u) kept(%u, %u)\n", nsqa, ncqa, ctrlr->base.nsqa,
	       ctrlr->base.ncqa);

	if (ctrlr->base.nsqa != nsqa || ctrlr->base.ncqa != ncqa) {
		fprintf(stderr,
			"FAIL: the controller keeps (%u, %u), Get Features gives (%u, %u)\n",
			ctrlr->base.nsqa, ctrlr->base.ncqa, nsqa, ncqa);
		return -EIO;
	}

	nqpairs = nsqa < ncqa ? nsqa : ncqa;
	if (nqpairs > NQPAIRS_MAX) {
		printf("SKIP: %u qpairs allocated, more than the %d this heap holds\n", nqpairs,
		       NQPAIRS_MAX);
		return 0;
	}

	for (uint32_t i = 0; i <= nqpairs; ++i) {
		err = nvme_controller_create_io_qpair_dmamem(
			ctrlr, &held[i].qp, DEPTH, heap, &held[i].sq_offset, &held[i].cq_offset,
			&held[i].prp_offset);
		if (err) {
			break;
		}
		created++;
	}

	if (!err) {
		fprintf(stderr, "FAIL: created %u qpairs, %u allocated\n", created, nqpairs);
		err = -EIO;
	} else if (created != nqpairs || err != -ENOMEM) {
		fprintf(stderr, "FAIL: created %u of %u qpairs, then err(%d), expected -ENOMEM\n",
			created, nqpairs, err);
		err = -EIO;
	} else {
		printf("OK: created %u qpairs, then -ENOMEM\n", created);
		err = 0;
	}

	for (uint32_t i = 0; i < created; ++i) {
		if (nvme_controller_delete_io_qpair_dmamem(ctrlr, &held[i].qp, heap,
							   held[i].sq_offset, held[i].cq_offset,
							   held[i].prp_offset)) {
			fprintf(stderr, "FAIL: nvme_controller_delete_io_qpair_dmamem(%u)\n", i);
			err = err ? err : -EIO;
		}
	}

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
	size_t hugepgsz = 2ULL * 1024 * 1024;
	// A qpair takes 4 MiB of PRP scratch for its request pool, and the admin qpair one more
	size_t nhugepages = (NQPAIRS_MAX + 2) * 2 + 8;
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

	err = run(&ctrlr, &heap);

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
