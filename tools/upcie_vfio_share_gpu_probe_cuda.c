// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * Delegated MMIO probe, CUDA flavour
 *
 * See tools/README.md.
 */

#include <upcie/upcie_cuda.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#define SHARE_PROBE_REGISTER_NAME "cuMemHostRegister(IOMEMORY)"
#define SHARE_PROBE_READ_NAME "kernel read"

int
upcie_probe_read_mmio(const void *mmio, uint32_t *out);

static char g_why[64];

static const char *
share_probe_why(void)
{
	return g_why;
}

static int
share_probe_rt_init(void)
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
share_probe_register(void *addr, size_t nbytes)
{
	CUresult cr = cuMemHostRegister(addr, nbytes, CU_MEMHOSTREGISTER_IOMEMORY);
	const char *name = NULL;

	if (cr == CUDA_SUCCESS) {
		return 0;
	}

	cuGetErrorName(cr, &name);
	snprintf(g_why, sizeof(g_why), "%s", name ? name : "FAILED");

	return (int)cr;
}

/**
 * The read comes from an SM, since the copy engine is not what rings a
 * doorbell.
 */
static int
share_probe_read(const void *addr, uint32_t *out)
{
	return upcie_probe_read_mmio(addr, out);
}

#include "vfio_share_gpu_probe.h"

int
main(int argc, char *argv[])
{
	return share_probe_main(argc, argv);
}
