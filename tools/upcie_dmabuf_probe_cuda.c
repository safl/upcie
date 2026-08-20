// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * dma-buf export probe, CUDA flavour
 *
 * See tools/README.md.
 */

#include <upcie/upcie_cuda.h>

static int
dmabuf_probe_export(int *fd, uint64_t va, size_t nbytes)
{
	CUresult cr = cuMemGetHandleForAddressRange(fd, (CUdeviceptr)va, nbytes,
						    CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD, 0);

	return cr == CUDA_SUCCESS ? 0 : (int)cr;
}

static int
dmabuf_probe_alloc(uint64_t *va, size_t nbytes)
{
	CUdeviceptr ptr = 0;
	CUresult cr = cuMemAlloc(&ptr, nbytes);

	*va = (uint64_t)ptr;

	return cr == CUDA_SUCCESS ? 0 : (int)cr;
}

static int
dmabuf_probe_addrrange(uint64_t *base, size_t *size, uint64_t va)
{
	CUdeviceptr b = 0;
	CUresult cr = cuMemGetAddressRange(&b, size, (CUdeviceptr)va);

	*base = (uint64_t)b;

	return cr == CUDA_SUCCESS ? 0 : (int)cr;
}

#include "dmabuf_probe.h"

int
main(void)
{
	struct cudamem_config config = {0};
	CUcontext ctx;
	CUdevice dev;
	int version = 0;

	/* cuCtxCreate has changed arity across CUDA releases; the primary
	 * context is stable and is what the runtime API uses anyway. */
	if (cuInit(0) || cuDriverGetVersion(&version) || cuDeviceGet(&dev, 0) ||
	    cuDevicePrimaryCtxRetain(&ctx, dev) || cuCtxSetCurrent(ctx)) {
		printf("CUDA initialization FAILED\n");
		return EXIT_FAILURE;
	}

	if (cudamem_config_init(&config, 0)) {
		printf("cudamem_config_init() FAILED\n");
		return EXIT_FAILURE;
	}

	printf("cuda:\n");
	printf("  driver_version: %d\n", version);
	printf("  pagesize: %d\n", config.pagesize);
	printf("  device_pagesize: %d\n", config.device_pagesize);
	printf("  alloc_granularity: %zu\n", config.alloc_granularity);

	return dmabuf_probe_run(64UL << 20) ? EXIT_FAILURE : EXIT_SUCCESS;
}
