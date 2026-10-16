// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * dmamem_memfd with plain pages and at a fixed IOVA
 * =================================================
 *
 * Constructs a dmamem over a plain (4 KiB page) memfd, once where the kernel
 * picks the IOVA and once at an IOVA fixed by the caller, on an IOAS with no
 * device attached. Checks the CPU view is writable, that the fixed IOVA is
 * honoured, and that destroying the dmamem gives the IOVA back, by mapping
 * at it again.
 *
 * Needs neither a device nor hugepages, only /dev/iommu and a memlock limit
 * that allows the mlock. Prints "OK" and exits 0 on success.
 */
#include <upcie/upcie.h>

#define TEST_SIZE (64 * 1024)
#define TEST_IOVA (1ULL << 32)

static int
check(struct dmamem *dmem, uint64_t want_iova)
{
	if (!dmem->cpu_va || dmem->size != TEST_SIZE) {
		fprintf(stderr, "FAIL: cpu_va(%p) size(%zu)\n", dmem->cpu_va, dmem->size);
		return -EINVAL;
	}
	if (want_iova && dmem->base_iova != want_iova) {
		fprintf(stderr, "FAIL: base_iova(0x%" PRIx64 ") != 0x%" PRIx64 "\n",
			dmem->base_iova, want_iova);
		return -EINVAL;
	}
	if (dmamem_offset_to_iova(dmem, 4096) != dmem->base_iova + 4096) {
		fprintf(stderr, "FAIL: offset translation\n");
		return -EINVAL;
	}

	memset(dmem->cpu_va, 0x5A, TEST_SIZE);
	if (((uint8_t *)dmem->cpu_va)[TEST_SIZE - 1] != 0x5A) {
		fprintf(stderr, "FAIL: write/read-back mismatch\n");
		return -EIO;
	}

	return 0;
}

static int
run(struct iommufd *iommufd)
{
	const uint32_t rw = IOMMU_IOAS_MAP_READABLE | IOMMU_IOAS_MAP_WRITEABLE;
	struct dmamem dmem = {0};
	int err;

	err = dmamem_from_memfd(&dmem, iommufd, TEST_SIZE, 4096);
	if (err) {
		fprintf(stderr, "FAIL: dmamem_from_memfd(4096) err(%d)\n", err);
		return err;
	}
	err = check(&dmem, 0);
	dmamem_destroy(&dmem);
	if (err) {
		return err;
	}

	err = dmamem_from_memfd(&dmem, iommufd, TEST_SIZE, 8192);
	if (err != -EINVAL) {
		fprintf(stderr, "FAIL: dmamem_from_memfd(8192) err(%d), want -EINVAL\n", err);
		dmamem_destroy(&dmem);
		return -EINVAL;
	}

	for (int i = 0; i < 2; i++) {
		err = dmamem_from_memfd_at(&dmem, iommufd, TEST_SIZE, 4096,
					   rw | IOMMU_IOAS_MAP_FIXED_IOVA, TEST_IOVA);
		if (err) {
			fprintf(stderr, "FAIL: dmamem_from_memfd_at() pass(%d) err(%d)%s\n", i,
				err, i ? "; destroy did not free the IOVA" : "");
			return err;
		}
		err = check(&dmem, TEST_IOVA);
		dmamem_destroy(&dmem);
		if (err) {
			return err;
		}
	}

	printf("OK\n");
	return 0;
}

int
main(void)
{
	struct iommufd iommufd = {0};
	int err;

	err = iommufd_open(&iommufd);
	if (err) {
		fprintf(stderr, "FAIL: iommufd_open() err(%d): %s\n", err, strerror(-err));
		return 1;
	}

	err = iommufd_ioas_alloc(&iommufd);
	if (err) {
		fprintf(stderr, "FAIL: iommufd_ioas_alloc() err(%d): %s\n", err, strerror(-err));
		iommufd_close(&iommufd);
		return 1;
	}

	err = run(&iommufd);

	iommufd_destroy(&iommufd, iommufd.ioas_id);
	iommufd_close(&iommufd);
	return err ? 1 : 0;
}
