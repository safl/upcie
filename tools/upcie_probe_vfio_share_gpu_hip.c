// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * Delegated MMIO probe, HIP flavour
 *
 * ROCm has no kernel-compilation path in this project, so the device-side read
 * goes through hipMemcpyDtoH rather than an SM. That is weaker evidence than
 * the CUDA flavour gives, and it is enough for the question actually being
 * asked, which is whether hipHostRegister accepts I/O memory at all.
 *
 * See tools/README.md.
 */

#include <upcie/upcie_hip.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#define SHARE_PROBE_REGISTER_NAME "hipHostRegister(IoMemory)"
#define SHARE_PROBE_READ_NAME "copy-engine read"

static char g_why[64];

static const char *
share_probe_why(void)
{
	return g_why;
}

static int
share_probe_rt_init(void)
{
	if (hipInit(0) || hipSetDevice(0)) {
		return -EIO;
	}

	return 0;
}

static int
share_probe_register(void *addr, size_t nbytes)
{
	hipError_t herr = hipHostRegister(addr, nbytes, hipHostRegisterIoMemory);

	if (herr == hipSuccess) {
		return 0;
	}

	snprintf(g_why, sizeof(g_why), "%s", hipGetErrorName(herr));

	return (int)herr;
}

static int
share_probe_read(const void *addr, uint32_t *out)
{
	void *devptr = NULL;
	hipError_t herr;

	herr = hipHostGetDevicePointer(&devptr, (void *)addr, 0);
	if (herr != hipSuccess) {
		snprintf(g_why, sizeof(g_why), "%s", hipGetErrorName(herr));
		return (int)herr;
	}

	herr = hipMemcpyDtoH(out, devptr, 2 * sizeof(uint32_t));

	return herr == hipSuccess ? 0 : (int)herr;
}

#include "probe_vfio_share_gpu.h"

int
main(int argc, char *argv[])
{
	return share_probe_main(argc, argv);
}
