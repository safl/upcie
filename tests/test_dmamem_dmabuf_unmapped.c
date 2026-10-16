// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * dmamem_from_dmabuf without an iommufd
 * =====================================
 *
 * Makes a dma-buf of a memfd through /dev/udmabuf and hands it to
 * dmamem_from_dmabuf() with no iommufd handle. The dmamem must hold the fd
 * and map nothing: base_iova stays 0, the CPU view udmabuf offers works,
 * and dmamem_destroy() closes the fd.
 *
 * Needs /dev/udmabuf and nothing else; exits 77 when it is absent. Prints
 * "OK" and exits 0 on success.
 */
#include <linux/udmabuf.h>

#include <upcie/upcie.h>

#define TEST_SIZE (64 * 1024)

static int
udmabuf_of(int memfd, size_t size)
{
	struct udmabuf_create create = {0};
	int dev, fd;

	dev = open("/dev/udmabuf", O_RDWR);
	if (dev < 0) {
		return -errno;
	}

	create.memfd = memfd;
	create.size = size;
	fd = ioctl(dev, UDMABUF_CREATE, &create);
	if (fd < 0) {
		fd = -errno;
	}

	close(dev);
	return fd;
}

int
main(void)
{
	struct dmamem dmem = {0};
	int memfd, fd, err;

	if (access("/dev/udmabuf", R_OK | W_OK)) {
		printf("SKIP: /dev/udmabuf is not available: %s\n", strerror(errno));
		return 77;
	}

	memfd = syscall(SYS_memfd_create, "upcie-test", MFD_ALLOW_SEALING);
	if (memfd < 0 || ftruncate(memfd, TEST_SIZE) || fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK)) {
		fprintf(stderr, "FAIL: sealed memfd: %s\n", strerror(errno));
		return 1;
	}

	fd = udmabuf_of(memfd, TEST_SIZE);
	if (fd < 0) {
		fprintf(stderr, "FAIL: UDMABUF_CREATE: %s\n", strerror(-fd));
		return 1;
	}

	err = dmamem_from_dmabuf(&dmem, NULL, fd, TEST_SIZE);
	if (err) {
		fprintf(stderr, "FAIL: dmamem_from_dmabuf(NULL) err(%d)\n", err);
		close(fd);
		return 1;
	}

	if (dmem.fd != fd || dmem.iommufd || dmem.base_iova || dmem.size != TEST_SIZE ||
	    !dmem.owned) {
		fprintf(stderr, "FAIL: unexpected dmamem\n");
		dmamem_pp(&dmem);
		return 1;
	}

	if (!dmem.cpu_va) {
		fprintf(stderr, "FAIL: no CPU view of a udmabuf\n");
		return 1;
	}
	memset(dmem.cpu_va, 0xA5, TEST_SIZE);
	if (((uint8_t *)dmem.cpu_va)[TEST_SIZE - 1] != 0xA5) {
		fprintf(stderr, "FAIL: write/read-back mismatch\n");
		return 1;
	}

	dmamem_destroy(&dmem);
	if (fcntl(fd, F_GETFD) != -1 || errno != EBADF) {
		fprintf(stderr, "FAIL: dmamem_destroy() left the dma-buf fd open\n");
		return 1;
	}

	close(memfd);
	printf("OK\n");
	return 0;
}
