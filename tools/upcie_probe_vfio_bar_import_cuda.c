// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * vfio BAR dma-buf import probe, CUDA flavour
 *
 * See tools/README.md.
 */

#include <upcie/upcie_cuda.h>

static CUexternalMemory g_extmem;
static CUdeviceptr g_devptr;
static CUcontext g_ctx;

static int
bar_import_probe_rt_init(void)
{
	CUdevice dev;

	if (cuInit(0) || cuDeviceGet(&dev, 0) || cuDevicePrimaryCtxRetain(&g_ctx, dev) ||
	    cuCtxSetCurrent(g_ctx)) {
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
bar_import_probe_note(const char *call, CUresult cr)
{
	const char *name = NULL;

	cuGetErrorName(cr, &name);
	snprintf(g_why + strlen(g_why), sizeof(g_why) - strlen(g_why), "%s%s=%s(%d)",
		 g_why[0] ? ", " : "", call, name ? name : "?", (int)cr);
}

static int
bar_import_probe_import(int dmabuf_fd, size_t nbytes, uint64_t *devptr)
{
	CUDA_EXTERNAL_MEMORY_HANDLE_DESC handle = {0};
	CUDA_EXTERNAL_MEMORY_BUFFER_DESC buffer = {0};
	CUmemGenericAllocationHandle mem;
	CUresult cr;

	handle.handle.fd = dmabuf_fd;
	handle.size = nbytes;

/* CU_EXTERNAL_MEMORY_HANDLE_TYPE_DMABUF_FD is an enumerator rather than a
 * macro, so the guard has to be on the release that introduced it. */
#if CUDA_VERSION >= 11070
	/* The handle type that names what this descriptor is. OPAQUE_FD is
	 * tried after it because it is what a reader of the older
	 * documentation reaches for, and the two fail differently. */
	handle.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_DMABUF_FD;

	cr = cuImportExternalMemory(&g_extmem, &handle);
	if (cr == CUDA_SUCCESS) {
		goto mapped;
	}
	bar_import_probe_note("import(DMABUF_FD)", cr);
#endif

	handle.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;

	cr = cuImportExternalMemory(&g_extmem, &handle);
	if (cr != CUDA_SUCCESS) {
		bar_import_probe_note("import(OPAQUE_FD)", cr);

		/* The other way in: the VMM allocator's shareable handle. It
		 * expects one of its own exports rather than an arbitrary
		 * dma-buf, so this is asked as a question, not as a fallback
		 * that is expected to work. */
		cr = cuMemImportFromShareableHandle(&mem, (void *)(uintptr_t)dmabuf_fd,
						    CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR);
		bar_import_probe_note("cuMemImportFromShareableHandle", cr);

		return (int)cr ? (int)cr : -EIO;
	}

#if CUDA_VERSION >= 11070
mapped:
#endif

	buffer.offset = 0;
	buffer.size = nbytes;

	cr = cuExternalMemoryGetMappedBuffer(&g_devptr, g_extmem, &buffer);
	if (cr != CUDA_SUCCESS) {
		bar_import_probe_note("cuExternalMemoryGetMappedBuffer", cr);
		return (int)cr;
	}

	*devptr = (uint64_t)g_devptr;

	return 0;
}

static int
bar_import_probe_read(uint64_t devptr, size_t nbytes, void *dst)
{
	CUresult cr = cuMemcpyDtoH(dst, (CUdeviceptr)devptr, nbytes);

	return cr == CUDA_SUCCESS ? 0 : (int)cr;
}

static void
bar_import_probe_release(void)
{
	if (g_extmem) {
		cuDestroyExternalMemory(g_extmem);
		g_extmem = NULL;
	}
}

/**
 * Does this runtime import a dma-buf it exported itself?
 *
 * Separates "will not import this descriptor" from "does not import
 * descriptors", which is the difference between a limitation of the vfio
 * exporter and one of the GPU stack.
 */
static int
bar_import_probe_selftest(char *why, size_t why_nbytes)
{
#if CUDA_VERSION >= 11070
	CUDA_EXTERNAL_MEMORY_HANDLE_DESC handle = {0};
	CUexternalMemory extmem = NULL;
	const char *name = NULL;
	CUdeviceptr ptr = 0;
	size_t nbytes = 2 << 20;
	CUresult cr;
	int fd = -1;

	cr = cuMemAlloc(&ptr, nbytes);
	if (cr != CUDA_SUCCESS) {
		snprintf(why, why_nbytes, "cuMemAlloc failed");
		return -EIO;
	}

	cr = cuMemGetHandleForAddressRange(&fd, ptr, nbytes, CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD,
					   0);
	if (cr != CUDA_SUCCESS) {
		cuGetErrorName(cr, &name);
		snprintf(why, why_nbytes, "export=%s(%d)", name ? name : "?", (int)cr);
		cuMemFree(ptr);
		return -EIO;
	}

	handle.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_DMABUF_FD;
	handle.handle.fd = fd;
	handle.size = nbytes;

	cr = cuImportExternalMemory(&extmem, &handle);
	cuGetErrorName(cr, &name);
	snprintf(why, why_nbytes, "own dma-buf import=%s(%d)", name ? name : "?", (int)cr);

	if (cr == CUDA_SUCCESS) {
		cuDestroyExternalMemory(extmem);
	}
	cuMemFree(ptr);

	return cr == CUDA_SUCCESS ? 0 : -EIO;
#else
	snprintf(why, why_nbytes, "runtime predates the dma-buf handle type");
	return -ENOTSUP;
#endif
}

#include "probe_vfio_bar_import.h"

int
main(int argc, char *argv[])
{
	return bar_import_probe_main(argc, argv);
}
