// SPDX-License-Identifier: GPL-2.0
/*
 * dmabuf_import_unload - what a legacy import holds after the process is gone
 * ==========================================================================
 *
 * DMABUF_IMPORT_ATTACH keys its imports in a table that outlives the file they
 * were made on, so a process that exits without a DETACH leaves its import
 * standing and the exporter's memory pinned. Only module unload can release it,
 * and unload used to release nothing: the descriptors were left in the tree and
 * their attachments left pointing at importer_ops in module text that rmmod
 * then freed.
 *
 * This attaches a large buffer with the legacy ioctl and exits without
 * detaching, so the import is standing when it returns. It cannot check the
 * outcome itself, since that needs the module unloaded, so the caller does:
 *
 *   A=$(awk '/MemAvailable/{print $2}' /proc/meminfo)
 *   ./dmabuf_import_unload
 *   B=$(awk '/MemAvailable/{print $2}' /proc/meminfo)   # A-B is the buffer
 *   rmmod dmabuf_import
 *   C=$(awk '/MemAvailable/{print $2}' /proc/meminfo)   # C-B must be too
 *
 * C-B near zero is the bug: the memory is pinned for as long as the machine is
 * up, with nothing left that could free it.
 *
 * Target-side (needs the stock /dev/udmabuf for UDMABUF_CREATE and the module
 * at /dev/dmabuf_import). /dev/udmabuf caps a buffer at size_limit_mb, 64 by
 * default, which is below what this asks for; raise it first. Exit 0 once the
 * import is standing, 1 if it could not be made.
 */
#define _GNU_SOURCE

#include <fcntl.h>
#include <linux/memfd.h>
#include <linux/udmabuf.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../module/dmabuf_import.h"

#define BUFFER_SIZE (512UL * 1024 * 1024)

int main(void)
{
	struct udmabuf_create create = {0};
	struct dmabuf_import_attach attach = {0};
	int udmabuf_fd, memfd, dmabuf_fd, import_fd;

	udmabuf_fd = open("/dev/udmabuf", O_RDWR);
	if (udmabuf_fd < 0) {
		perror("open /dev/udmabuf");
		return 1;
	}

	memfd = memfd_create("dmabuf-import-unload", MFD_ALLOW_SEALING);
	if (memfd < 0) {
		perror("memfd_create");
		return 1;
	}
	if (fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK) || ftruncate(memfd, BUFFER_SIZE)) {
		perror("seal/ftruncate");
		return 1;
	}

	create.memfd = memfd;
	create.size = BUFFER_SIZE;
	dmabuf_fd = ioctl(udmabuf_fd, UDMABUF_CREATE, &create);
	close(memfd); /* the dma-buf holds what it needs of it */
	if (dmabuf_fd < 0) {
		perror("UDMABUF_CREATE (raise /sys/module/udmabuf/parameters/size_limit_mb)");
		return 1;
	}

	import_fd = open(DMABUF_IMPORT_DEVPATH, O_RDWR);
	if (import_fd < 0) {
		perror("open " DMABUF_IMPORT_DEVPATH);
		return 1;
	}

	attach.fd = dmabuf_fd;
	if (ioctl(import_fd, DMABUF_IMPORT_ATTACH, &attach)) {
		perror("DMABUF_IMPORT_ATTACH");
		return 1;
	}

	printf("%zu MiB imported as %u segments; exiting without detaching it\n",
	       BUFFER_SIZE / (1024 * 1024), attach.count);

	return 0;
}
