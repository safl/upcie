// SPDX-License-Identifier: BSD-3-Clause
/*
 * Where does a large GPU allocation end up once it is imported?
 *
 * Importing a dma-buf can succeed while the exporter quietly moves the buffer
 * to system memory, which leaves everything downstream working and no longer
 * peer-to-peer. This watches the driver's own VRAM and GTT accounting across
 * each step, so a migration shows up as numbers moving rather than as an
 * address somebody had to interpret. Several GiB makes the change unmistakable.
 *
 * It also writes a pattern through the GPU and reads it back afterwards, since
 * memory that migrated must still hold its contents; a probe that only watched
 * counters could not tell a migration from a free.
 *
 *   ./dmabuf_import_probe_hip <gpu-bdf> [size-MiB] [importer-bdf]
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "dmabuf_import_accounting.h"
#include "dmabuf_import_placement.h"

#define __HIP_PLATFORM_AMD__
#include <hip/hip_runtime.h>

#define PATTERN 0x5a

int
main(int argc, char *argv[])
{
	struct gpu_accounting base;
	struct dmabuf_import_attach attach;
	const char *bdf = argc > 1 ? argv[1] : NULL;
	size_t nbytes = (argc > 2 ? (size_t)atoll(argv[2]) : 4096) << 20;
	const char *importer = argc > 3 ? argv[3] : NULL;
	unsigned char *check = NULL;
	void *vaddr = NULL;
	int import_fd = -1, dmabuf_fd = -1, err = 0;

	if (!bdf) {
		printf("usage: %s <gpu-bdf> [size-MiB] [importer-bdf]\n", argv[0]);
		return 1;
	}

	printf("allocating %.1f GiB on %s, importing for %s\n\n",
	       nbytes / (1024.0 * 1024 * 1024), bdf, importer ? importer : "the misc device");
	base = gpu_accounting_read(bdf);
	gpu_accounting_pr("baseline", bdf, base);

	if (hipInit(0) || hipSetDevice(0)) {
		printf("FAILED: no HIP device\n");
		return 1;
	}

	if (hipMalloc(&vaddr, nbytes)) {
		printf("FAILED: hipMalloc(%zu)\n", nbytes);
		return 1;
	}
	if (hipMemset(vaddr, PATTERN, nbytes)) {
		printf("FAILED: hipMemset()\n");
		err = 1;
		goto exit;
	}
	gpu_accounting_pr("after hipMalloc", bdf, base);

	if (hipMemGetHandleForAddressRange(&dmabuf_fd, (hipDeviceptr_t)vaddr, nbytes,
					   hipMemRangeHandleTypeDmaBufFd, 0)) {
		printf("FAILED: hipMemGetHandleForAddressRange()\n");
		err = 1;
		goto exit;
	}
	gpu_accounting_pr("after dma-buf export", bdf, base);

	import_fd = open(DMABUF_IMPORT_DEVPATH, O_RDWR);
	if (import_fd < 0) {
		printf("FAILED: open(%s), errno %d; is the module loaded?\n",
		       DMABUF_IMPORT_DEVPATH, errno);
		err = 1;
		goto exit;
	}

	/* Importing for the device that will actually read the memory is what
	 * lets the exporter answer about peer-to-peer; without one it is asked
	 * on behalf of a misc device with no bus path. */
	if (importer) {
		struct dmabuf_import_attach_bdf att;

		memset(&att, 0, sizeof(att));
		att.fd = dmabuf_fd;
		snprintf(att.bdf, sizeof(att.bdf), "%s", importer);
		if (ioctl(import_fd, DMABUF_IMPORT_ATTACH_BDF, &att)) {
			printf("FAILED: DMABUF_IMPORT_ATTACH_BDF(%s), errno %d (%s)\n", importer,
			       errno, strerror(errno));
			err = 1;
			goto exit;
		}
	} else {
		memset(&attach, 0, sizeof(attach));
		attach.fd = dmabuf_fd;
		if (ioctl(import_fd, DMABUF_IMPORT_ATTACH, &attach)) {
			printf("FAILED: DMABUF_IMPORT_ATTACH, errno %d (%s)\n", errno,
			       strerror(errno));
			err = 1;
			goto exit;
		}
	}
	gpu_accounting_pr("after import+pin", bdf, base);
	printf("\n");
	dmabuf_import_placement(import_fd, dmabuf_fd, bdf, "by address:");

	/* Memory that moved must still hold what was written, or the counters
	 * above described a free rather than a migration. */
	check = malloc(nbytes);
	if (!check) {
		/* Not a pass: the counters above cannot tell a migration from a
		 * free, which is the whole reason for reading it back. */
		printf("contents:        NOT CHECKED, could not allocate %zu bytes\n", nbytes);
		err = 1;
	} else {
		size_t bad = 0;

		memset(check, 0, nbytes);
		if (hipMemcpy(check, vaddr, nbytes, hipMemcpyDeviceToHost)) {
			printf("contents:        unreadable after import\n");
			err = 1;
		} else {
			for (size_t i = 0; i < nbytes; ++i) {
				if (check[i] != PATTERN) {
					++bad;
				}
			}
			printf("contents:        %s (%zu bytes differ)\n",
			       bad ? "CORRUPTED" : "intact", bad);
			err = err || (bad != 0);
		}
		free(check);
	}

	printf("\n");
	ioctl(import_fd, DMABUF_IMPORT_DETACH, &dmabuf_fd);
	gpu_accounting_pr("after detach", bdf, base);

exit:
	if (import_fd >= 0) {
		close(import_fd);
	}
	if (dmabuf_fd >= 0) {
		close(dmabuf_fd);
	}
	hipFree(vaddr);
	gpu_accounting_pr("after hipFree", bdf, base);

	return err;
}
