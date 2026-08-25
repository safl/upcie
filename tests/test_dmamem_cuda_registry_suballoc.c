// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * Register several CUDA allocations, not just the first
 *
 * The registry indexes its table by granule, so it needs the allocation a
 * registration falls inside to start on a granule boundary. A runtime packs
 * suballocations into a larger block at arbitrary offsets, so only the one
 * that happens to land on the boundary satisfies that. Allocating a handful
 * without freeing in between is what produces the packing, and a consumer
 * holding more than one buffer is the ordinary case rather than a corner one.
 */

#include <upcie/upcie_cuda.h>
#include <cuda.h>

int
main(void)
{
	const size_t sizes[] = {4096, 16384, 131072, 4096, 16384, 131072};
	const size_t nbuffers = sizeof(sizes) / sizeof(*sizes);
	struct cudamem_config config = {0};
	struct cudamem_heap heap = {0};
	struct dmamem dmem = {0};
	CUdeviceptr buffers[6] = {0};
	CUdevice cu_dev;
	CUcontext cu_ctx;
	int nerr = 0, err;

	err = cuInit(0);
	if (err) {
		printf("# FAILED: cuInit(); err(%d)\n", err);
		return err;
	}

	err = cuDeviceGet(&cu_dev, 0);
	if (err) {
		printf("# FAILED: cuDeviceGet(); err(%d)\n", err);
		return err;
	}

	err = cudamem_ctx_create(&cu_ctx, cu_dev);
	if (err) {
		printf("# FAILED: cudamem_ctx_create(); err(%d)\n", err);
		return err;
	}

	err = cudamem_config_init(&config, 0);
	if (err) {
		printf("# FAILED: cudamem_config_init(); err(%d)\n", err);
		return err;
	}

	err = cudamem_heap_init(&heap, 1024 * 1024 * 64ULL, &config);
	if (err) {
		printf("# FAILED: cudamem_heap_init(); err(%d)\n", err);
		return -err;
	}

	err = dmamem_from_cuda_registry(&dmem, &heap, 0);
	if (err) {
		printf("# FAILED: dmamem_from_cuda_registry(); err(%d)\n", err);
		return -err;
	}

	/* Allocated up front and freed only at the end, so the runtime packs
	 * them into one block rather than handing back the same address */
	for (size_t i = 0; i < nbuffers; ++i) {
		err = cuMemAlloc(&buffers[i], sizes[i]);
		if (err) {
			printf("# FAILED: cuMemAlloc(%zu); err(%d)\n", sizes[i], err);
			return err;
		}
	}

	for (size_t i = 0; i < nbuffers; ++i) {
		uint64_t gran_off = (uint64_t)buffers[i] & ((2UL << 20) - 1);

		err = dmamem_register(&dmem, (void *)buffers[i], sizes[i]);
		printf("# [%zu] size %-7zu granule-offset 0x%06" PRIx64 " -> err(%d)\n", i,
		       sizes[i], gran_off, err);
		if (err) {
			nerr += 1;
			continue;
		}

		err = dmamem_unregister(&dmem, (void *)buffers[i]);
		if (err) {
			printf("# FAILED: dmamem_unregister(); err(%d)\n", err);
			nerr += 1;
		}
	}

	for (size_t i = 0; i < nbuffers; ++i) {
		cuMemFree(buffers[i]);
	}
	dmamem_destroy(&dmem);
	cudamem_heap_term(&heap);

	if (nerr) {
		printf("# FAILED: %d of %zu allocations could not be registered\n", nerr,
		       nbuffers);
		return 1;
	}

	printf("# LGTM: every allocation registered\n");

	return 0;
}
