// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * dmamem NVMe IDENTIFY CONTROLLER smoketest
 * =========================================
 *
 * End-to-end proof of the vfio-cdev + iommufd + dmamem + NVMe admin path:
 *
 *   1. Open iommufd and allocate an IOAS.
 *   2. Reserve a hugepage-backed dmamem, initialize a dmamem_heap over it.
 *   3. Open the NVMe controller via vfio-cdev + iommufd, attach it to the
 *      IOAS, mmap BAR0, program the admin queue with SQ/CQ carved from the
 *      dmamem_heap, enable the controller and wait for CSTS.RDY.
 *   4. Carve a 4 KiB IDENTIFY response buffer from the same heap. Issue
 *      IDENTIFY CONTROLLER (CNS=0x01) with PRP1 = base_iova + offset.
 *      Wait for the completion.
 *   5. Print serial + model + firmware from the response. Tear down.
 *
 * Requires the target NVMe controller to be bound to vfio-pci and its
 * vfio cdev at /dev/vfio/devices/vfioN. Pass that cdev path as argv[1]
 * (or set NVME_DMAMEM_CDEV=/dev/vfio/devices/vfio0 in the environment) so
 * the test does not have to resolve BDF to cdev on its own.
 *
 * On success prints "OK: SN='...' MN='...' FR='...'" and exits 0.
 */
#define _UPCIE_WITH_NVME
#include <upcie/upcie.h>

#define IDENTIFY_BUF_SIZE 4096
#define NVME_ADMIN_IDENTIFY 0x06
#define NVME_IDENTIFY_CNS_CTRL 0x01

static void
trim_ascii_field(char *dst, const char *src, size_t n)
{
	size_t end = n;

	memcpy(dst, src, n);
	dst[n] = '\0';
	while (end && (dst[end - 1] == ' ' || dst[end - 1] == '\0')) {
		dst[--end] = '\0';
	}
}

static int
run_identify(struct nvme_controller *ctrlr, struct dmamem_heap *heap)
{
	struct nvme_command cmd = {0};
	struct nvme_completion cpl = {0};
	size_t buf_offset = 0;
	uint64_t buf_iova;
	uint8_t *buf;
	char sn[21], mn[41], fr[9];
	int err;

	err = dmamem_heap_alloc_aligned(heap, IDENTIFY_BUF_SIZE, 4096, &buf_offset);
	if (err) {
		fprintf(stderr, "FAIL: dmamem_heap_alloc_aligned(identify) err(%d)\n", err);
		return err;
	}

	buf = dmamem_heap_at_va(heap, buf_offset);
	buf_iova = dmamem_heap_at_iova(heap, buf_offset);
	memset(buf, 0, IDENTIFY_BUF_SIZE);

	cmd.opc = NVME_ADMIN_IDENTIFY;
	cmd.cid = 1;
	cmd.prp1 = buf_iova;
	cmd.cdw10 = NVME_IDENTIFY_CNS_CTRL;

	err = nvme_qpair_enqueue(&ctrlr->aq, &cmd);
	if (err) {
		fprintf(stderr, "FAIL: nvme_qpair_enqueue() err(%d)\n", err);
		goto out_free;
	}

	nvme_qpair_sqdb_update(&ctrlr->aq);

	err = nvme_qpair_reap_cpl(&ctrlr->aq, 5000, &cpl);
	if (err) {
		fprintf(stderr, "FAIL: nvme_qpair_reap_cpl() err(%d)\n", err);
		goto out_free;
	}

	if ((cpl.status >> 1) & 0x7FF) {
		fprintf(stderr, "FAIL: IDENTIFY CQE status(0x%x) sc(0x%x) sct(0x%x)\n", cpl.status,
			(cpl.status >> 1) & 0xFF, (cpl.status >> 9) & 0x7);
		err = -EIO;
		goto out_free;
	}

	trim_ascii_field(sn, (const char *)&buf[4], 20);
	trim_ascii_field(mn, (const char *)&buf[24], 40);
	trim_ascii_field(fr, (const char *)&buf[64], 8);

	printf("OK: SN='%s' MN='%s' FR='%s'\n", sn, mn, fr);

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
	size_t hugepgsz = 2ULL * 1024 * 1024;
	size_t nhugepages = 4;
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

	err = run_identify(&ctrlr, &heap);

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
