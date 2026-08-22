// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * dma-buf export probe, HIP flavour
 *
 * See tools/README.md.
 */

#include <upcie/upcie_hip.h>

static int
dmabuf_probe_export(int *fd, uint64_t va, size_t nbytes)
{
	hipError_t cr = hipMemGetHandleForAddressRange(fd, (hipDeviceptr_t)va, nbytes,
						       hipMemRangeHandleTypeDmaBufFd, 0);

	return cr == hipSuccess ? 0 : (int)cr;
}

static int
dmabuf_probe_alloc(uint64_t *va, size_t nbytes)
{
	void *ptr = NULL;
	hipError_t cr = hipMalloc(&ptr, nbytes);

	*va = (uint64_t)ptr;

	return cr == hipSuccess ? 0 : (int)cr;
}

static int
dmabuf_probe_addrrange(uint64_t *base, size_t *size, uint64_t va)
{
	void *b = NULL;
	hipError_t cr = hipMemGetAddressRange((hipDeviceptr_t *)&b, size, (hipDeviceptr_t)va);

	*base = (uint64_t)b;

	return cr == hipSuccess ? 0 : (int)cr;
}

#include "dmabuf_probe.h"

int
main(void)
{
	struct hipmem_config config = {0};
	int version = 0;

	if (hipInit(0) || hipSetDevice(0) || hipDriverGetVersion(&version)) {
		printf("HIP initialization FAILED\n");
		return EXIT_FAILURE;
	}

	if (hipmem_config_init(&config, 0)) {
		printf("hipmem_config_init() FAILED\n");
		return EXIT_FAILURE;
	}

	printf("hip:\n");
	printf("  driver_version: %d\n", version);
	printf("  pagesize: %d\n", config.pagesize);
	printf("  device_pagesize: %d\n", config.device_pagesize);
	printf("  alloc_granularity: %zu\n", config.alloc_granularity);

	return dmabuf_probe_run(64UL << 20) ? EXIT_FAILURE : EXIT_SUCCESS;
}
