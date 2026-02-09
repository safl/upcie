#include <upcie/upcie_cuda.h>
#include <cuda.h>

int main(void)
{
	struct dmabuf dmabuf = {0};
	const size_t pagesize = 1 << 16; // 64k
	const size_t size = 8 * pagesize;
	uint64_t *phys_lut = NULL;
	CUdeviceptr vaddr;
	CUdevice cu_dev;
	CUcontext cu_ctx;
	size_t nphys;
	int dmabuf_fd, err;

	err = cuInit(0);
	if (err) {
		printf("# FAILED: cuInit(); err(%d)\n", err);
		return err;
	}

	err = cuDeviceGet(&cu_dev, 0); // GPU ID 0
	if (err) {
		printf("# FAILED: cuDeviceGet(); err(%d)\n", err);
		return err;
	}

	err = cuCtxCreate(&cu_ctx, 0, cu_dev);
	if (err) {
		printf("# FAILED: cuCtxCreate(); err(%d)\n", err);
		goto exit;
	}
	
	err = cuMemAlloc(&vaddr, size);
	if (err) {
		UPCIE_DEBUG("FAILED: cuMemAlloc(heap), err: %d", err);
		goto exit;
	}

	err = cuMemGetHandleForAddressRange(&dmabuf_fd, vaddr, size, CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD, 0);
	if (err) {
		UPCIE_DEBUG("FAILED: cuMemGetHandleForAddressRange(heap), err: %d", err);
		goto exit;
	}

	nphys = size / pagesize;

	phys_lut = calloc(nphys, sizeof(uint64_t));
	if (!phys_lut) {
		err = -errno;
		printf("# FAILED: calloc(phys_lut); errno(%d)\n", err);
		goto exit;
	}

	err = dmabuf_attach(dmabuf_fd, &dmabuf);
	if (err) {
		printf("# FAILED: dmabuf_attach(); err(%d)\n", err);
		goto exit;
	}

	dmabuf_pp(&dmabuf);

	err = dmabuf_get_lut(&dmabuf, nphys, phys_lut, pagesize);
	if (err) {
		printf("# FAILED: dmabuf_get_lut(); err(%d)\n", err);
		dmabuf_detach(&dmabuf);
		goto exit;
	}

	printf("LUT:\n");
	printf("  nphys: %zu\n", nphys);
	printf("  phys_lut:\n");
	for (size_t i = 0; i < nphys; i++) {
		printf("  - 0x%" PRIx64 "\n", phys_lut[i]);
	}

	dmabuf_detach(&dmabuf);

exit:
	cuMemFree(vaddr);
	free(phys_lut);
	cuCtxDestroy(cu_ctx);
	return err;
}