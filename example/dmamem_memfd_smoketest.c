// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * dmamem_memfd smoketest
 * ======================
 *
 * Exercises the round-trip of the memfd-backed dmamem: open /dev/iommu,
 * allocate an IOAS, construct a dmamem via dmamem_from_memfd, verify the
 * cpu_va is mapped and writable, verify base_iova + offset arithmetic
 * matches the CPU VA math, allocate a sub-range through dmamem_heap,
 * write and read back through the CPU view, tear down.
 *
 * This test does not require an NVMe device; it only exercises iommufd
 * and the memfd exporter. It requires:
 *
 *   - /dev/iommu accessible to the running user
 *   - a hugepage pool with at least the requested count reserved
 *   - a memlock ulimit that allows the mmap+mlock
 *
 * On success the program prints "OK" and exits with 0. Any failure logs
 * to stderr and exits non-zero.
 */
#include <upcie/upcie.h>

static int
smoketest(struct iommufd *iommufd, uint32_t ioas_id, size_t hugepgsz, size_t nhugepages)
{
	struct dmamem dmem = {0};
	struct dmamem_heap heap = {0};
	size_t heap_size = hugepgsz * nhugepages;
	size_t alloc_size = 4096;
	size_t offset = 0;
	uint64_t iova_via_offset, iova_via_va;
	void *va;
	int err;

	err = dmamem_from_memfd(&dmem, iommufd, ioas_id, heap_size, hugepgsz);
	if (err) {
		fprintf(stderr, "FAIL: dmamem_from_memfd() err(%d)\n", err);
		return err;
	}
	dmamem_pp(&dmem);

	if (!dmem.cpu_va) {
		fprintf(stderr, "FAIL: dmem.cpu_va is NULL\n");
		err = -EINVAL;
		goto out;
	}
	if (dmem.size != heap_size) {
		fprintf(stderr, "FAIL: dmem.size(%zu) != heap_size(%zu)\n", dmem.size, heap_size);
		err = -EINVAL;
		goto out;
	}
	if (!dmem.base_iova) {
		fprintf(stderr, "FAIL: dmem.base_iova is 0 (unexpected on modern iommufd)\n");
		err = -EINVAL;
		goto out;
	}

	err = dmamem_heap_init(&heap, &dmem, 4096);
	if (err) {
		fprintf(stderr, "FAIL: dmamem_heap_init() err(%d)\n", err);
		goto out;
	}

	err = dmamem_heap_alloc(&heap, alloc_size, &offset);
	if (err) {
		fprintf(stderr, "FAIL: dmamem_heap_alloc() err(%d)\n", err);
		goto out_heap;
	}

	iova_via_offset = dmamem_heap_at_iova(&heap, offset);
	va = dmamem_heap_at_va(&heap, offset);
	iova_via_va = dmamem_va_to_iova(&dmem, va);

	if (iova_via_offset != iova_via_va) {
		fprintf(stderr,
			"FAIL: IOVA arithmetic disagrees: via_offset(0x%" PRIx64
			") != via_va(0x%" PRIx64 ")\n",
			iova_via_offset, iova_via_va);
		err = -EINVAL;
		goto out_heap;
	}

	/* Touch the sub-range through the CPU VA. */
	memset(va, 0xAB, alloc_size);
	if (((uint8_t *)va)[0] != 0xAB || ((uint8_t *)va)[alloc_size - 1] != 0xAB) {
		fprintf(stderr, "FAIL: write/read-back mismatch\n");
		err = -EIO;
		goto out_heap;
	}

	printf("OK: heap_size=%zu, base_iova=0x%" PRIx64 ", offset=%zu, alloc_iova=0x%" PRIx64
	       "\n",
	       heap_size, dmem.base_iova, offset, iova_via_offset);

	dmamem_heap_free(&heap, offset);

out_heap:
	dmamem_heap_term(&heap);
out:
	dmamem_destroy(&dmem);
	return err;
}

int
main(int argc, char *argv[])
{
	struct iommufd iommufd = {0};
	uint32_t ioas_id = 0;
	size_t hugepgsz = 2ULL * 1024 * 1024;
	size_t nhugepages = 4;
	int err;

	(void)argc;
	(void)argv;

	err = iommufd_open(&iommufd);
	if (err) {
		fprintf(stderr, "FAIL: iommufd_open() err(%d): %s\n", err, strerror(-err));
		return 1;
	}

	err = iommufd_ioas_alloc(&iommufd, &ioas_id);
	if (err) {
		fprintf(stderr, "FAIL: iommufd_ioas_alloc() err(%d): %s\n", err, strerror(-err));
		iommufd_close(&iommufd);
		return 1;
	}

	err = smoketest(&iommufd, ioas_id, hugepgsz, nhugepages);

	if (iommufd_destroy(&iommufd, ioas_id)) {
		fprintf(stderr, "WARN: iommufd_destroy(ioas)\n");
	}
	iommufd_close(&iommufd);
	return err ? 1 : 0;
}
