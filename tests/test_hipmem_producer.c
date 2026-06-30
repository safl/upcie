// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * hipmem producer check (no udmabuf-import needed)
 * ================================================
 *
 * Exercises the AMD/HIP half of the hipmem backend without the dma-buf import
 * path: query the device config (HIP + BAR1) and allocate GPU memory, then
 * export it as a P2P dma-buf via HSA. This is the producer that hipmem_heap
 * feeds into dmabuf_attach()/dmabuf_get_lut(); the full heap additionally needs
 * the udmabuf-import kernel module, which this check deliberately avoids so it
 * runs on a stock ROCm install.
 */
#include <upcie/upcie_hip.h>

int
main(void)
{
	const size_t size = 2 * 1024 * 1024;
	struct hipmem_config config = {0};
	void *vaddr = NULL;
	int dmabuf_fd = -1, err;

	err = hipmem_config_init(&config, 0);
	if (err) {
		printf("FAILED: hipmem_config_init(); err(%d)\n", err);
		return 1;
	}
	hipmem_config_pp(&config);

	err = hipMalloc(&vaddr, size);
	if (err) {
		printf("FAILED: hipMalloc(); err(%d)\n", err);
		return 1;
	}

	err = hipMemGetHandleForAddressRange(&dmabuf_fd, (hipDeviceptr_t)vaddr, size,
					     hipMemRangeHandleTypeDmaBufFd, 0);
	if (err) {
		printf("FAILED: hipMemGetHandleForAddressRange(); err(%d)\n", err);
		hipFree(vaddr);
		return 1;
	}

	printf("PASS: hipmem config + GPU dma-buf export; bar1=%zu device_memsize=%zu fd=%d\n",
	       config.bar1_size, config.device_memsize, dmabuf_fd);

	if (dmabuf_fd >= 0) {
		close(dmabuf_fd);
	}
	hipFree(vaddr);

	return 0;
}
