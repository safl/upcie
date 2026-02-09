#include <upcie/upcie_cuda.h>
#include <cuda.h>

int main(void)
{
	struct cudamem_heap heap = {0};
	const size_t nbuffers = 10;
	void *buffers[nbuffers];
	CUdevice cu_dev;
	CUcontext cu_ctx;
	int err;

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
		return err;
	}

	err = cudamem_heap_init(&heap, 1024 * 1024 * 256ULL);
	if (err) {
		printf("# FAILED: cudamem_heap_init(); err(%d)\n", err);
		return -err;
	}

	cudamem_heap_pp(&heap);

	for (size_t i = 0; i < nbuffers; i++) {
		buffers[i] = cudamem_heap_block_alloc(&heap, 4);
		if (!buffers[i]) {
			return -errno;
		}
	}

	cudamem_heap_pp(&heap);

	for (size_t i = 0; i < nbuffers; i++) {
		cudamem_heap_block_free(&heap, buffers[i]);
	}

	cudamem_heap_pp(&heap);

	cudamem_heap_term(&heap);

	cuCtxDestroy(cu_ctx);

	return 0;
}
