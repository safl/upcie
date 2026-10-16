// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * dmamem_from_memfd without an iommufd, and its ranges mapped elsewhere
 * =====================================================================
 *
 * Makes a dmamem of a plain-page memfd with no iommufd handle: it must map
 * nothing, keep base_iova 0, give a writable CPU view, and seal the memfd
 * against resizing. A second dmamem holds a duplicate of the fd with
 * dmamem_from_memfd_fd(), as a process that received it would, and maps its
 * second page over a reservation with dmamem_map_at(): the mapping lands
 * where asked and shares the pages with the first dmamem's view. A range past
 * the end is refused, and dmamem_destroy() closes the held fd.
 *
 * Needs no device and no /dev/iommu. Prints "OK" and exits 0 on success.
 */
#include <upcie/upcie.h>

#define TEST_SIZE (64 * 1024)

int
main(void)
{
	struct dmamem dmem = {0}, held = {0};
	char *resv, *p;
	int fd, seals, err;

	err = dmamem_from_memfd(&dmem, NULL, TEST_SIZE, 4096);
	if (err) {
		fprintf(stderr, "FAIL: dmamem_from_memfd(NULL) err(%d)\n", err);
		return 1;
	}
	if (dmem.fd < 0 || !dmem.cpu_va || dmem.base_va != dmem.cpu_va || dmem.base_iova ||
	    dmem.iommufd || dmem.size != TEST_SIZE || !dmem.owned ||
	    dmem.backing != DMAMEM_BACKING_MEMFD) {
		fprintf(stderr, "FAIL: unexpected dmamem\n");
		dmamem_pp(&dmem);
		return 1;
	}
	if (dmamem_offset_to_iova(&dmem, 4096) != 4096) {
		fprintf(stderr, "FAIL: an offset is not its own IOVA\n");
		return 1;
	}

	seals = fcntl(dmem.fd, F_GET_SEALS);
	if (seals < 0 || !(seals & F_SEAL_SHRINK) || !(seals & F_SEAL_GROW) || (seals & F_SEAL_WRITE)) {
		fprintf(stderr, "FAIL: seals 0x%x, not shrink and grow alone\n", seals);
		return 1;
	}
	if (!ftruncate(dmem.fd, TEST_SIZE / 2)) {
		fprintf(stderr, "FAIL: a sealed memfd shrank\n");
		return 1;
	}

	memset(dmem.cpu_va, 0x5A, TEST_SIZE);

	fd = fcntl(dmem.fd, F_DUPFD_CLOEXEC, 0);
	if (fd < 0 || dmamem_from_memfd_fd(&held, fd, TEST_SIZE)) {
		fprintf(stderr, "FAIL: dmamem_from_memfd_fd()\n");
		return 1;
	}
	if (held.cpu_va || held.fd != fd || !held.owned) {
		fprintf(stderr, "FAIL: unexpected held dmamem\n");
		dmamem_pp(&held);
		return 1;
	}
	if (dmamem_from_memfd_fd(&(struct dmamem){0}, fd, 2 * TEST_SIZE) != -EINVAL) {
		fprintf(stderr, "FAIL: a size past the memfd taken\n");
		return 1;
	}

	resv = mmap(NULL, 2 * 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (resv == MAP_FAILED) {
		fprintf(stderr, "FAIL: reserving: %s\n", strerror(errno));
		return 1;
	}
	p = dmamem_map_at(&held, resv + 4096, 4096, 4096);
	if (p != resv + 4096) {
		fprintf(stderr, "FAIL: dmamem_map_at() gave %p, not %p\n", (void *)p, (void *)(resv + 4096));
		return 1;
	}
	if (p[0] != 0x5A) {
		fprintf(stderr, "FAIL: the mapping does not see what the first view wrote\n");
		return 1;
	}
	p[1] = 0x7E;
	if (((char *)dmem.cpu_va)[4096 + 1] != 0x7E) {
		fprintf(stderr, "FAIL: the first view does not see what the mapping wrote\n");
		return 1;
	}
	if (dmamem_map_at(&held, resv, TEST_SIZE - 4096, 2 * 4096) != MAP_FAILED || errno != EINVAL) {
		fprintf(stderr, "FAIL: a range past the end mapped\n");
		return 1;
	}
	munmap(resv, 2 * 4096);

	dmamem_destroy(&held);
	if (fcntl(fd, F_GETFD) != -1 || errno != EBADF) {
		fprintf(stderr, "FAIL: dmamem_destroy() left the held fd open\n");
		return 1;
	}
	dmamem_destroy(&dmem);

	printf("OK\n");
	return 0;
}
