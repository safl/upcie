// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * vfio BAR dma-buf import probe, HIP flavour
 *
 * See tools/README.md.
 */

#include <upcie/upcie_hip.h>

static hipExternalMemory_t g_extmem;
static void *g_devptr;

static int
bar_import_probe_rt_init(void)
{
	if (hipInit(0) || hipSetDevice(0)) {
		return -EIO;
	}

	return 0;
}

static char g_why[256];

static const char *
bar_import_probe_why(void)
{
	return g_why;
}

static void
bar_import_probe_note(const char *call, hipError_t herr)
{
	snprintf(g_why + strlen(g_why), sizeof(g_why) - strlen(g_why), "%s%s=%s(%d)",
		 g_why[0] ? ", " : "", call, hipGetErrorName(herr), (int)herr);
}

static int
bar_import_probe_import(int dmabuf_fd, size_t nbytes, uint64_t *devptr)
{
	hipExternalMemoryHandleDesc handle = {0};
	hipExternalMemoryBufferDesc buffer = {0};
	hipError_t herr;

	handle.type = hipExternalMemoryHandleTypeOpaqueFd;
	handle.handle.fd = dmabuf_fd;
	handle.size = nbytes;

	herr = hipImportExternalMemory(&g_extmem, &handle);
	if (herr != hipSuccess) {
		bar_import_probe_note("hipImportExternalMemory", herr);
		return (int)herr;
	}

	buffer.offset = 0;
	buffer.size = nbytes;

	herr = hipExternalMemoryGetMappedBuffer(&g_devptr, g_extmem, &buffer);
	if (herr != hipSuccess) {
		bar_import_probe_note("hipExternalMemoryGetMappedBuffer", herr);
		return (int)herr;
	}

	*devptr = (uint64_t)g_devptr;

	return 0;
}

static int
bar_import_probe_read(uint64_t devptr, size_t nbytes, void *dst)
{
	hipError_t herr = hipMemcpyDtoH(dst, (void *)devptr, nbytes);

	return herr == hipSuccess ? 0 : (int)herr;
}

static void
bar_import_probe_release(void)
{
	if (g_extmem) {
		hipDestroyExternalMemory(g_extmem);
		g_extmem = NULL;
	}
}

static int
bar_import_probe_selftest(char *why, size_t why_nbytes)
{
	snprintf(why, why_nbytes, "no dma-buf handle type in this runtime");

	return -ENOTSUP;
}

#include "probe_vfio_bar_import.h"

int
main(int argc, char *argv[])
{
	return bar_import_probe_main(argc, argv);
}
