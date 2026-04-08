// SPDX-License-Identifier: BSD-3-Clause

// Include CUDA and minimal upcie headers directly to avoid pulling in
// non-CUDA headers via the bundle, which have C-only void* cast idioms.
#include <cuda_runtime.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>

#include <upcie/nvme/nvme_command.h>
#include <upcie/nvme/nvme_qpair_cuda.h>

/**
 * Submit NVMe IOs from the GPU and reap their completions.
 *
 * Handles single IO, batched IO, multi-queue IO, and multi-round IO through
 * the CUDA grid and block dimensions combined with num_ios:
 *
 *   Single IO:     grid=1, block=1, num_ios=1
 *   Batch IO:      grid=1, block=<queue depth>, num_ios=<queue depth>
 *   Multi-queue:   grid=<num queues>, block=<queue depth>, num_ios=<num queues * queue depth>
 *   Multi-round:   grid=<num queues>, block=<queue depth>, num_ios=N (any N >= 1)
 *
 * Each block maps to one queue (qps[blockIdx.x]). Each thread handles one
 * command per round. Commands and results are laid out in round-major order:
 * round 0 occupies [0, stride), round 1 [stride, 2*stride), etc., where
 * stride = gridDim.x * blockDim.x. The last round may be partial; threads
 * with no remaining work participate in barriers but do not submit IOs.
 *
 * @param qps     Device array of queue-pair pointers, one per block
 * @param cmds    Flat device array of commands (num_ios entries)
 * @param results Flat device array of per-command results (num_ios entries)
 * @param num_ios Total number of IOs to submit
 */
extern "C" __global__ void
nvme_io(struct nvme_qpair_cuda **qps, struct nvme_command *cmds, int *results,
	uint32_t num_ios)
{
	size_t bid = blockIdx.x;
	size_t tid = threadIdx.x;
	size_t stride = gridDim.x * blockDim.x;
	size_t num_rounds = (num_ios + stride - 1) / stride;

	for (size_t r = 0; r < num_rounds; r++) {
		size_t block_start = (size_t)r * stride + bid * blockDim.x;
		size_t remaining = (block_start < (size_t)num_ios) ? (size_t)num_ios - block_start : 0;
		size_t batch_size = remaining < blockDim.x ? remaining : blockDim.x;
		size_t gid = block_start + tid;
		size_t cmd_idx = tid < batch_size ? gid : block_start;

		int result = nvme_qpair_cuda_io(qps[bid], &cmds[cmd_idx], tid, batch_size);

		if (gid < (size_t)num_ios) {
			results[gid] = result;
		}
	}
}

/**
 * Launch nvme_io and synchronize.
 *
 * @param qps     Device array of queue-pair pointers, one per block
 * @param cmds    Flat device array of commands (num_ios entries)
 * @param results Flat device array of per-command results (num_ios entries)
 * @param num_ios Total number of IOs to submit
 * @param grid    Number of thread blocks (one per queue)
 * @param block   Number of threads per block (queue depth)
 *
 * @return cudaSuccess on success, cudaError_t on failure.
 */
extern "C" cudaError_t
nvme_io_launch(struct nvme_qpair_cuda **qps, struct nvme_command *cmds, int *results,
	       uint32_t num_ios, unsigned int grid, unsigned int block)
{
	nvme_io<<<grid, block>>>(qps, cmds, results, num_ios);
	return cudaDeviceSynchronize();
}
