// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * dmamem_export_udmabuf of a memfd dmamem
 * =======================================
 *
 * Makes a dmamem of a plain-page memfd with no iommufd, exports its second
 * and third pages as a dma-buf through /dev/udmabuf, and holds the dma-buf
 * with dmamem_from_dmabuf() and no iommufd, as test_hostmem_dmabuf.c makes
 * one by hand: the dma-buf's CPU view shows the memfd's pages at the offset
 * asked for, writes through it reach the memfd, it outlives the dmamem it was
 * made of, and a range past the end or of a dmamem not of a memfd is refused.
 *
 * Needs /dev/udmabuf and nothing else; exits 77 when it is absent. Prints
 * "OK" and exits 0 on success.
 */
#include <upcie/upcie.h>

#define TEST_SIZE (64 * 1024)

int
main(void)
{
	struct dmamem dmem = {0}, buf = {0}, other = {0};
	int fd, err;

	if (access("/dev/udmabuf", R_OK | W_OK)) {
		printf("SKIP: /dev/udmabuf is not available: %s\n", strerror(errno));
		return 77;
	}

	err = dmamem_from_memfd(&dmem, NULL, TEST_SIZE, 4096);
	if (err) {
		fprintf(stderr, "FAIL: dmamem_from_memfd(NULL) err(%d)\n", err);
		return 1;
	}
	memset(dmem.cpu_va, 0x11, 4096);
	memset((char *)dmem.cpu_va + 4096, 0x22, 2 * 4096);

	if (dmamem_export_udmabuf(&dmem, TEST_SIZE - 4096, 2 * 4096, &fd) != -EINVAL || fd != -1) {
		fprintf(stderr, "FAIL: a range past the end exported\n");
		return 1;
	}
	other.fd = -1;
	other.backing = DMAMEM_BACKING_DMABUF;
	other.size = TEST_SIZE;
	if (dmamem_export_udmabuf(&other, 0, 4096, &fd) != -EINVAL) {
		fprintf(stderr, "FAIL: a dmamem of no memfd exported\n");
		return 1;
	}

	err = dmamem_export_udmabuf(&dmem, 4096, 2 * 4096, &fd);
	if (err || fd < 0) {
		fprintf(stderr, "FAIL: dmamem_export_udmabuf() err(%d)\n", err);
		return 1;
	}
	if (!(fcntl(fd, F_GETFD) & FD_CLOEXEC)) {
		fprintf(stderr, "FAIL: the dma-buf is not close-on-exec\n");
		return 1;
	}

	err = dmamem_from_dmabuf(&buf, NULL, fd, 2 * 4096);
	if (err || !buf.cpu_va) {
		fprintf(stderr, "FAIL: dmamem_from_dmabuf(NULL) err(%d), cpu_va %p\n", err, buf.cpu_va);
		return 1;
	}
	if (((uint8_t *)buf.cpu_va)[0] != 0x22 || ((uint8_t *)buf.cpu_va)[2 * 4096 - 1] != 0x22) {
		fprintf(stderr, "FAIL: the dma-buf does not show the memfd at the offset\n");
		return 1;
	}
	((uint8_t *)buf.cpu_va)[5] = 0x33;
	if (((uint8_t *)dmem.cpu_va)[4096 + 5] != 0x33) {
		fprintf(stderr, "FAIL: a write through the dma-buf did not reach the memfd\n");
		return 1;
	}

	dmamem_destroy(&dmem);
	if (((uint8_t *)buf.cpu_va)[5] != 0x33) {
		fprintf(stderr, "FAIL: the dma-buf lost its pages with the dmamem\n");
		return 1;
	}
	dmamem_destroy(&buf);

	printf("OK\n");
	return 0;
}
