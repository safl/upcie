// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * VRAM into an IOAS, HIP flavour
 *
 * See tools/README.md.
 */

#include <upcie/upcie_hip.h>

static void *g_ptr;

static int
vram_ioas_probe_rt_init(void)
{
	if (hipInit(0) || hipSetDevice(0)) {
		return -EIO;
	}

	return 0;
}

static int
vram_ioas_probe_alloc_export(size_t nbytes, int *dmabuf_fd)
{
	hipError_t herr;

	herr = hipMalloc(&g_ptr, nbytes);
	if (herr != hipSuccess) {
		return (int)herr;
	}

	herr = hipMemGetHandleForAddressRange(dmabuf_fd, (hipDeviceptr_t)g_ptr, nbytes,
					      hipMemRangeHandleTypeDmaBufFd, 0);

	return herr == hipSuccess ? 0 : (int)herr;
}

#include "vram_ioas_probe.h"

int
main(void)
{
	return vram_ioas_probe_run(2 << 20);
}
