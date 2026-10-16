// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * vfio_map_region_at over a reservation
 * =====================================
 *
 * Reserves an address range PROT_NONE and maps a file over its second half
 * with vfio_map_region_at(), as a CPU view of device memory is placed over a
 * reserved device pointer. A memfd stands in for the vfio device fd: the call
 * is a plain mmap of whatever fd it is given, so this needs no device.
 * Checks the mapping lands at the address asked for and reaches the file at
 * the offset asked for.
 *
 * Prints "OK" and exits 0 on success.
 */
#include <upcie/upcie.h>

#define TEST_SIZE (64 * 1024)

int
main(void)
{
	uint8_t byte = 0;
	char *resv, *p;
	int fd;

	fd = syscall(SYS_memfd_create, "upcie-test", 0);
	if (fd < 0 || ftruncate(fd, 2 * TEST_SIZE)) {
		fprintf(stderr, "FAIL: memfd: %s\n", strerror(errno));
		return 1;
	}

	resv = mmap(NULL, 2 * TEST_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
		    -1, 0);
	if (resv == MAP_FAILED) {
		fprintf(stderr, "FAIL: reserving: %s\n", strerror(errno));
		return 1;
	}

	p = vfio_map_region_at(fd, resv + TEST_SIZE, TEST_SIZE, TEST_SIZE);
	if (p != resv + TEST_SIZE) {
		fprintf(stderr, "FAIL: vfio_map_region_at() gave %p, not %p: %s\n", (void *)p,
			(void *)(resv + TEST_SIZE), p == MAP_FAILED ? strerror(errno) : "moved");
		return 1;
	}

	p[1] = 0x7E;
	if (pread(fd, &byte, 1, TEST_SIZE + 1) != 1 || byte != 0x7E) {
		fprintf(stderr, "FAIL: the store did not reach the file at the offset\n");
		return 1;
	}

	munmap(resv, 2 * TEST_SIZE);
	close(fd);
	printf("OK\n");
	return 0;
}
