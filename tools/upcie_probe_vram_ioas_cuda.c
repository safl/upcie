// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * VRAM into an IOAS, CUDA flavour
 *
 * See tools/README.md.
 */

#include <upcie/upcie_cuda.h>

static CUdeviceptr g_ptr;

static int
vram_ioas_probe_rt_init(void)
{
	CUcontext ctx;
	CUdevice dev;

	if (cuInit(0) || cuDeviceGet(&dev, 0) || cuDevicePrimaryCtxRetain(&ctx, dev) ||
	    cuCtxSetCurrent(ctx)) {
		return -EIO;
	}

	return 0;
}

static int
vram_ioas_probe_alloc_export(size_t nbytes, int *dmabuf_fd)
{
	CUresult cr;

	cr = cuMemAlloc(&g_ptr, nbytes);
	if (cr != CUDA_SUCCESS) {
		return (int)cr;
	}

	cr = cuMemGetHandleForAddressRange(dmabuf_fd, g_ptr, nbytes,
					   CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD, 0);

	return cr == CUDA_SUCCESS ? 0 : (int)cr;
}

#include "probe_vram_ioas.h"

int
main(void)
{
	return vram_ioas_probe_run(2 << 20);
}
