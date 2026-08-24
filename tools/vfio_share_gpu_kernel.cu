// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * Read two dwords of MMIO from an SM, for upcie_vfio_share_gpu_probe_cuda.
 *
 * The copy engine is not what rings a doorbell, so the read that answers the
 * question has to come from a kernel rather than from cuMemcpyDtoH().
 */

#include <cuda_runtime.h>
#include <stdint.h>

__global__ static void
upcie_probe_read_mmio_kernel(const volatile uint32_t *src, uint32_t *dst)
{
	dst[0] = src[0];
	dst[1] = src[1];
}

extern "C" int
upcie_probe_read_mmio(const void *mmio, uint32_t *out)
{
	uint32_t *dst = NULL;
	cudaError_t cerr;

	cerr = cudaMalloc((void **)&dst, 2 * sizeof(uint32_t));
	if (cerr != cudaSuccess) {
		return (int)cerr;
	}

	upcie_probe_read_mmio_kernel<<<1, 1>>>((const volatile uint32_t *)mmio, dst);

	cerr = cudaDeviceSynchronize();
	if (cerr != cudaSuccess) {
		cudaFree(dst);
		return (int)cerr;
	}

	cerr = cudaMemcpy(out, dst, 2 * sizeof(uint32_t), cudaMemcpyDeviceToHost);
	cudaFree(dst);

	return cerr == cudaSuccess ? 0 : (int)cerr;
}
