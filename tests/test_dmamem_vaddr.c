// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * dmamem_from_vaddr and dmamem_adopt_iova
 * =======================================
 *
 * On an IOAS with no device attached: maps anonymous memory with
 * dmamem_from_vaddr() where the kernel picks the IOVA and at a fixed one,
 * adopts a range mapped behind the dmamem's back with dmamem_adopt_iova(),
 * and checks that destroying either gives the IOVA back, by mapping at it
 * again, while leaving the caller's memory mapped.
 *
 * Needs /dev/iommu and nothing else. Prints "OK" and exits 0 on success.
 */
#include <upcie/upcie.h>

#define TEST_SIZE (64 * 1024)
#define TEST_IOVA (1ULL << 32)
#define TEST_IOVA_ADOPT (2ULL << 32)

static const uint32_t rw = IOMMU_IOAS_MAP_READABLE | IOMMU_IOAS_MAP_WRITEABLE;

/* 0 when [IOVA, IOVA + TEST_SIZE) is free in the IOAS, by mapping and unmapping it */
static int
iova_free(struct iommufd *iommufd, void *va, uint64_t iova)
{
	uint64_t at = iova;
	int err;

	err = iommufd_ioas_map(iommufd, (uint64_t)(uintptr_t)va, TEST_SIZE,
			       rw | IOMMU_IOAS_MAP_FIXED_IOVA, &at);
	if (err) {
		return err;
	}

	return iommufd_ioas_unmap(iommufd, iova, TEST_SIZE);
}

static int
from_vaddr(struct iommufd *iommufd, void *va)
{
	struct dmamem dmem = {0};
	int err;

	err = dmamem_from_vaddr(&dmem, NULL, va, TEST_SIZE, rw, 0);
	if (err != -EINVAL) {
		fprintf(stderr, "FAIL: dmamem_from_vaddr(NULL iommufd) err(%d), want -EINVAL\n",
			err);
		return -EINVAL;
	}

	err = dmamem_from_vaddr(&dmem, iommufd, va, TEST_SIZE, rw, 0);
	if (err) {
		fprintf(stderr, "FAIL: dmamem_from_vaddr() err(%d)\n", err);
		return err;
	}
	if (dmem.owned || dmem.fd != -1 || dmem.cpu_va != va || dmem.base_va != va ||
	    dmem.size != TEST_SIZE || dmem.backing != DMAMEM_BACKING_VADDR) {
		fprintf(stderr, "FAIL: unexpected dmamem\n");
		dmamem_pp(&dmem);
		dmamem_destroy(&dmem);
		return -EINVAL;
	}
	if (dmamem_va_to_iova(&dmem, (char *)va + 4096) != dmem.base_iova + 4096) {
		fprintf(stderr, "FAIL: translation\n");
		dmamem_destroy(&dmem);
		return -EINVAL;
	}
	dmamem_destroy(&dmem);

	for (int i = 0; i < 2; i++) {
		err = dmamem_from_vaddr(&dmem, iommufd, va, TEST_SIZE,
					rw | IOMMU_IOAS_MAP_FIXED_IOVA, TEST_IOVA);
		if (err) {
			fprintf(stderr, "FAIL: dmamem_from_vaddr(fixed) pass(%d) err(%d)%s\n", i,
				err, i ? "; destroy did not free the IOVA" : "");
			return err;
		}
		if (dmem.base_iova != TEST_IOVA) {
			fprintf(stderr, "FAIL: base_iova(0x%" PRIx64 ") is not the fixed IOVA\n",
				dmem.base_iova);
			dmamem_destroy(&dmem);
			return -EINVAL;
		}
		dmamem_destroy(&dmem);
	}

	return 0;
}

static int
adopt(struct iommufd *iommufd, void *va)
{
	struct dmamem dmem = {0};
	uint64_t at = TEST_IOVA_ADOPT;
	int err;

	err = iommufd_ioas_map(iommufd, (uint64_t)(uintptr_t)va, TEST_SIZE,
			       rw | IOMMU_IOAS_MAP_FIXED_IOVA, &at);
	if (err) {
		fprintf(stderr, "FAIL: iommufd_ioas_map() err(%d)\n", err);
		return err;
	}

	err = dmamem_adopt_iova(&dmem, iommufd, TEST_IOVA_ADOPT, TEST_SIZE);
	if (err) {
		fprintf(stderr, "FAIL: dmamem_adopt_iova() err(%d)\n", err);
		iommufd_ioas_unmap(iommufd, TEST_IOVA_ADOPT, TEST_SIZE);
		return err;
	}
	if (dmem.cpu_va || dmem.base_va || dmem.owned || dmem.backing != DMAMEM_BACKING_IOVA ||
	    dmamem_offset_to_iova(&dmem, 8192) != TEST_IOVA_ADOPT + 8192) {
		fprintf(stderr, "FAIL: unexpected dmamem\n");
		dmamem_pp(&dmem);
		dmamem_destroy(&dmem);
		return -EINVAL;
	}

	if (iova_free(iommufd, va, TEST_IOVA_ADOPT) != -EEXIST) {
		fprintf(stderr, "FAIL: the adopted range is not mapped\n");
		dmamem_destroy(&dmem);
		return -EINVAL;
	}
	dmamem_destroy(&dmem);

	err = iova_free(iommufd, va, TEST_IOVA_ADOPT);
	if (err) {
		fprintf(stderr, "FAIL: destroy did not unmap the adopted range err(%d)\n", err);
		return err;
	}

	return 0;
}

int
main(void)
{
	struct iommufd iommufd = {0};
	void *va;
	int err;

	va = mmap(NULL, TEST_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (va == MAP_FAILED) {
		fprintf(stderr, "FAIL: mmap(): %s\n", strerror(errno));
		return 1;
	}
	memset(va, 0x3C, TEST_SIZE);

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

	err = from_vaddr(&iommufd, va);
	if (!err) {
		err = adopt(&iommufd, va);
	}
	if (!err && ((uint8_t *)va)[TEST_SIZE - 1] != 0x3C) {
		fprintf(stderr, "FAIL: the caller's memory changed\n");
		err = -EIO;
	}

	iommufd_destroy(&iommufd, iommufd.ioas_id);
	iommufd_close(&iommufd);
	munmap(va, TEST_SIZE);

	if (!err) {
		printf("OK\n");
	}
	return err ? 1 : 0;
}
