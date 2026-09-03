// SPDX-License-Identifier: BSD-3-Clause
/*
 * Where does an imported dma-buf actually live?
 *
 * An exporter may satisfy an import by migrating the buffer to system memory.
 * That succeeds, the addresses look plausible, and I/O through them works, so
 * nothing downstream notices that it is no longer peer-to-peer. Comparing the
 * addresses against the exporter's BAR is not a sound check either: with an
 * IOMMU in the path they are IOVAs, and the comparison silently means nothing.
 *
 * What settles it is that host memory always has a struct page behind it and
 * device memory does not, which DMABUF_IMPORT_DESCRIBE reports and which holds
 * whether or not the addresses can be read.
 *
 * Run it with a GPU allocator to see whether that GPU's memory is reachable
 * peer-to-peer; the udmabuf case below is the negative control, since host
 * memory must never report as peer-to-peer.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/udmabuf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../dmabuf_import_placement.h"

#define MEMFD_NBYTES (2UL << 20)

/**
 * Import 'dmabuf_fd' and report how the kernel placed it
 */
static int
placement(int import_fd, int dmabuf_fd, const char *bdf, const char *what)
{
	struct dmabuf_import_attach attach;
	int verdict;

	memset(&attach, 0, sizeof(attach));
	attach.fd = dmabuf_fd;
	if (ioctl(import_fd, DMABUF_IMPORT_ATTACH, &attach)) {
		printf("%-16s attach failed, errno %d (%s)\n", what, errno, strerror(errno));
		return DMABUF_PLACEMENT_ERROR;
	}

	verdict = dmabuf_import_placement(import_fd, dmabuf_fd, bdf, what);
	ioctl(import_fd, DMABUF_IMPORT_DETACH, &dmabuf_fd);

	return verdict;
}

/**
 * A dma-buf over plain host memory, which must never report as peer-to-peer
 */
static int
host_dmabuf(void)
{
	struct udmabuf_create create;
	int memfd, udmabuf, fd;

	memfd = memfd_create("dmabuf_import_placement", MFD_ALLOW_SEALING);
	if (memfd < 0) {
		return -1;
	}
	if (ftruncate(memfd, MEMFD_NBYTES) || fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK)) {
		close(memfd);
		return -1;
	}

	udmabuf = open("/dev/udmabuf", O_RDWR);
	if (udmabuf < 0) {
		close(memfd);
		return -1;
	}

	memset(&create, 0, sizeof(create));
	create.memfd = memfd;
	create.offset = 0;
	create.size = MEMFD_NBYTES;

	fd = ioctl(udmabuf, UDMABUF_CREATE, &create);
	close(udmabuf);
	close(memfd);

	return fd;
}

int
main(int argc, char *argv[])
{
	int import_fd, host_fd, err = 0;

	(void)argc;
	(void)argv;

	import_fd = open(DMABUF_IMPORT_DEVPATH, O_RDWR);
	if (import_fd < 0) {
		printf("open(%s) failed, errno %d; is the module loaded?\n",
		       DMABUF_IMPORT_DEVPATH, errno);
		return 1;
	}

	/* The negative control: host memory reporting as peer-to-peer would mean
	 * the check itself is broken, so the GPU result could not be believed. */
	host_fd = host_dmabuf();
	if (host_fd < 0) {
		printf("%-18s skipped (no /dev/udmabuf)\n", "host memory");
	} else {
		if (placement(import_fd, host_fd, NULL, "host memory") != DMABUF_PLACEMENT_HOST) {
			printf("FAILED: host memory did not report as host memory\n");
			err = 1;
		}
		close(host_fd);
	}

	close(import_fd);

	return err;
}
