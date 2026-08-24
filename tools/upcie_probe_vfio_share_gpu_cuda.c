// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * Delegated MMIO probe, CUDA flavour
 *
 * A process can pass a vfio device fd to an unrelated process over SCM_RIGHTS,
 * and the receiver can map BAR0 and read a register. What that leaves untested
 * is the step the GPU-initiated NVMe path depends on: registering a window of
 * that received mapping as I/O memory with cuMemHostRegister(), and reaching
 * it from an SM.
 *
 * Two processes rather than a fork with a setuid in it. Dropping privilege is
 * not the same state as never having had it: the dropped process keeps the
 * supplementary groups it was started with unless they are cleared, and a uid
 * change clears the dumpable flag, either of which could be what a refusal is
 * really about. So the secondary is started separately, as whatever user it is
 * started as, and the two meet over a named socket.
 *
 * It reads CAP rather than writing a doorbell, so nothing here disturbs a
 * controller, and both sides print what they read.
 *
 * Usage:
 *   upcie_probe_vfio_share_gpu_cuda primary <cdev> <socket>
 *   upcie_probe_vfio_share_gpu_cuda secondary <socket>
 *   upcie_probe_vfio_share_gpu_cuda standalone <cdev>
 *
 * The standalone mode is the control: no delegation, the process opens the
 * device itself. Run as an ordinary user it says whether a refusal is about
 * the caller or about the descriptor having been passed.
 *
 * The primary serves one secondary and exits when it disconnects, so run it in
 * the background and start the secondary as the user in question, for example
 * with setpriv --reuid=1000 --regid=1000 --clear-groups.
 *
 * @file upcie_probe_vfio_share_gpu_cuda.c
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

#include "probe_vfio_share_gpu.h"

int
main(int argc, char *argv[])
{
	return share_probe_main(argc, argv);
}
