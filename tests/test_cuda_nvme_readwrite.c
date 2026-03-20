// SPDX-License-Identifier: BSD-3-Clause

#define _UPCIE_WITH_NVME
#include <upcie/upcie_cuda.h>

int nvme_io_launch(struct nvme_qpair_cuda **qps, struct nvme_command *cmds, int *results,
		   uint32_t num_ios, unsigned int grid, unsigned int block);

#define BUF_SIZE     (64 * 1024)  ///< One CUDA heap page per IO buffer
#define VERIFY_SIZE  512          ///< Bytes to fill and verify per buffer (min NVMe sector size)

struct rte {
	struct hostmem_config config;
	struct hostmem_heap heap;
	struct cudamem_config cuda_config;
	struct cudamem_heap cuda_heap;
	CUcontext cu_ctx;
};

struct nvme {
	struct nvme_controller ctrlr;
	struct nvme_qpair_cuda **ioqs;    ///< Host array of device queue-pair pointers
	struct nvme_qpair_cuda **cu_ioqs; ///< Device array of queue-pair pointers
	int num_queues;
	int queue_depth;
};

void
rte_term(struct rte *rte)
{
	hostmem_heap_term(&rte->heap);
	cudamem_heap_term(&rte->cuda_heap);
	cuCtxDestroy(rte->cu_ctx);
}

int
rte_init(struct rte *rte, size_t cuda_heap_size)
{
	CUdevice cu_dev;
	int err;

	err = hostmem_config_init(&rte->config);
	if (err) {
		printf("FAILED: hostmem_config_init(); err(%d)\n", err);
		return err;
	}

	err = hostmem_heap_init(&rte->heap, 1024 * 1024 * 128ULL, &rte->config);
	if (err) {
		printf("FAILED: hostmem_heap_init(); err(%d)\n", err);
		return err;
	}

	err = cuInit(0);
	if (err) {
		printf("FAILED: cuInit(); err(%d)\n", err);
		hostmem_heap_term(&rte->heap);
		return err;
	}

	err = cuDeviceGet(&cu_dev, 0);
	if (err) {
		printf("FAILED: cuDeviceGet(); err(%d)\n", err);
		hostmem_heap_term(&rte->heap);
		return err;
	}

	err = cuCtxCreate(&rte->cu_ctx, 0, cu_dev);
	if (err) {
		printf("FAILED: cuCtxCreate(); err(%d)\n", err);
		hostmem_heap_term(&rte->heap);
		return err;
	}

	err = cudamem_config_init(&rte->cuda_config, 0);
	if (err) {
		printf("FAILED: cudamem_config_init(); err(%d)\n", err);
		cuCtxDestroy(rte->cu_ctx);
		hostmem_heap_term(&rte->heap);
		return err;
	}

	err = cudamem_heap_init(&rte->cuda_heap, cuda_heap_size, &rte->cuda_config);
	if (err) {
		printf("FAILED: cudamem_heap_init(); err(%d)\n", err);
		cuCtxDestroy(rte->cu_ctx);
		hostmem_heap_term(&rte->heap);
		return err;
	}

	return 0;
}

void
nvme_term(struct nvme *nvme, struct rte *rte)
{
	for (int i = 0; i < nvme->num_queues; i++) {
		nvme_controller_cuda_delete_io_qpair(&nvme->ctrlr, nvme->ioqs[i],
						     &rte->cuda_heap);
		cuMemFree((CUdeviceptr)nvme->ioqs[i]);
	}

	cuMemFree((CUdeviceptr)nvme->cu_ioqs);
	free(nvme->ioqs);
	nvme_controller_close(&nvme->ctrlr);
}

int
nvme_init(struct nvme *nvme, const char *bdf, struct rte *rte, int num_queues, int queue_depth)
{
	struct nvme_completion cpl = {0};
	struct nvme_command cmd = {0};
	int err;

	nvme->num_queues = num_queues;
	nvme->queue_depth = queue_depth;

	err = nvme_controller_open(&nvme->ctrlr, bdf, &rte->heap);
	if (err) {
		printf("FAILED: nvme_controller_open(); err(%d)\n", err);
		return err;
	}

	cmd.opc = 0x6; ///< IDENTIFY
	cmd.cdw10 = 1; // CNS=1: Identify Controller

	err = nvme_qpair_submit_sync_contig_prps(&nvme->ctrlr.aq, nvme->ctrlr.heap,
						 nvme->ctrlr.buf, 4096, &cmd,
						 nvme->ctrlr.timeout_ms, &cpl);
	if (err) {
		printf("FAILED: nvme_qpair_submit_sync(); err(%d)\n", err);
		nvme_controller_close(&nvme->ctrlr);
		return err;
	}

	nvme->ioqs = calloc(num_queues, sizeof(*nvme->ioqs));
	if (!nvme->ioqs) {
		err = -errno;
		printf("FAILED: calloc(ioqs); err(%d)\n", err);
		nvme_controller_close(&nvme->ctrlr);
		return err;
	}

	for (int i = 0; i < num_queues; i++) {
		err = cuMemAlloc((CUdeviceptr *)&nvme->ioqs[i], sizeof(struct nvme_qpair_cuda));
		if (err) {
			printf("FAILED: cuMemAlloc(ioqs[%d]); CUresult(%d)\n", i, err);
			nvme_controller_close(&nvme->ctrlr);
			return err;
		}

		// NVMe queues hold at most depth-1 in-flight commands; add 1 so all
		// queue_depth threads can have a command outstanding simultaneously.
		err = nvme_controller_cuda_create_io_qpair(&nvme->ctrlr, nvme->ioqs[i],
							   queue_depth + 1, &rte->cuda_heap);
		if (err) {
			printf("FAILED: nvme_controller_cuda_create_io_qpair(%d); err(%d)\n", i, err);
			nvme_controller_close(&nvme->ctrlr);
			return err;
		}
	}

	err = cuMemAlloc((CUdeviceptr *)&nvme->cu_ioqs,
			 num_queues * sizeof(struct nvme_qpair_cuda *));
	if (err) {
		printf("FAILED: cuMemAlloc(cu_ioqs); CUresult(%d)\n", err);
		nvme_controller_close(&nvme->ctrlr);
		return err;
	}

	err = cuMemcpyHtoD((CUdeviceptr)nvme->cu_ioqs, nvme->ioqs,
			   num_queues * sizeof(struct nvme_qpair_cuda *));
	if (err) {
		printf("FAILED: cuMemcpyHtoD(cu_ioqs); CUresult(%d)\n", err);
		nvme_controller_close(&nvme->ctrlr);
		return err;
	}

	return 0;
}

int
prep_nvme_io(struct nvme *nvme, struct cudamem_heap *cuda_heap, uint8_t opc, void *buffers,
	     size_t num_ios)
{
	struct nvme_command *cmds;
	struct nvme_command *cu_cmds;
	int *results, *cu_results;
	uint32_t cu_num_ios = num_ios;
	int err;

	cmds = calloc(num_ios, sizeof(*cmds));
	if (!cmds) {
		err = -errno;
		printf("FAILED: calloc(cmds); err(%d)\n", err);
		return err;
	}

	results = calloc(num_ios, sizeof(*results));
	if (!results) {
		err = -errno;
		printf("FAILED: calloc(results); err(%d)\n", err);
		free(cmds);
		return err;
	}

	for (size_t gid = 0; gid < num_ios; gid++) {
		cmds[gid].nsid = 1;
		cmds[gid].opc = opc;
		cmds[gid].cdw10 = gid; ///< SLBA == global IO index
		cmds[gid].cdw12 = 0;   ///< NLB == 1 LBA
		cmds[gid].prp1 = cudamem_heap_block_vtp(
			cuda_heap, (uint8_t *)buffers + gid * BUF_SIZE);
	}

	err = cuMemAlloc((CUdeviceptr *)&cu_cmds, num_ios * sizeof(*cmds));
	if (err) {
		printf("FAILED: cuMemAlloc(cu_cmds); CUresult(%d)\n", err);
		free(cmds);
		free(results);
		return err;
	}

	err = cuMemcpyHtoD((CUdeviceptr)cu_cmds, cmds, num_ios * sizeof(*cmds));
	free(cmds);
	if (err) {
		printf("FAILED: cuMemcpyHtoD(cmds); CUresult(%d)\n", err);
		cuMemFree((CUdeviceptr)cu_cmds);
		free(results);
		return err;
	}

	err = cuMemAlloc((CUdeviceptr *)&cu_results, num_ios * sizeof(*results));
	if (err) {
		printf("FAILED: cuMemAlloc(cu_results); CUresult(%d)\n", err);
		cuMemFree((CUdeviceptr)cu_cmds);
		free(results);
		return err;
	}

	err = nvme_io_launch(nvme->cu_ioqs, cu_cmds, cu_results, cu_num_ios,
			     nvme->num_queues, nvme->queue_depth);
	if (err) {
		printf("FAILED: nvme_io_launch(); cudaError_t(%d)\n", err);
		cuMemFree((CUdeviceptr)cu_cmds);
		cuMemFree((CUdeviceptr)cu_results);
		free(results);
		return err;
	}

	err = cuMemcpyDtoH(results, (CUdeviceptr)cu_results, num_ios * sizeof(*results));
	cuMemFree((CUdeviceptr)cu_cmds);
	cuMemFree((CUdeviceptr)cu_results);
	if (err) {
		printf("FAILED: cuMemcpyDtoH(results); CUresult(%d)\n", err);
		free(results);
		return err;
	}

	for (size_t i = 0; i < num_ios; i++) {
		if (results[i]) {
			printf("FAILED: nvme_io[%zu]; result(%d)\n", i, results[i]);
			if (!err) {
				err = results[i];
			}
		}
	}

	free(results);
	return err;
}

int
main(int argc, char **argv)
{
	struct nvme nvme = {0};
	struct rte rte = {0};
	int num_queues, queue_depth;
	size_t num_ios;
	void *write_buf = NULL, *read_buf = NULL;	///< CUDA IO buffers
	uint8_t *expected = NULL, *actual = NULL;	///< HOST buffers for comparison
	int err;

	if (argc != 5) {
		printf("Usage: %s <PCI-BDF> <num-queues> <queue-depth> <num-ios>\n", argv[0]);
		return 1;
	}

	num_queues = atoi(argv[2]);
	queue_depth = atoi(argv[3]);
	num_ios = (size_t)atoi(argv[4]);

	err = rte_init(&rte, num_ios * BUF_SIZE * 2 + 8 * 1024 * 1024ULL);
	if (err) {
		printf("FAILED: rte_init(); err(%d)\n", err);
		return err;
	}

	err = nvme_init(&nvme, argv[1], &rte, num_queues, queue_depth);
	if (err) {
		printf("FAILED: nvme_init(); err(%d)\n", err);
		rte_term(&rte);
		return err;
	}

	write_buf = cudamem_heap_block_alloc(&rte.cuda_heap, num_ios * BUF_SIZE);
	if (!write_buf) {
		err = errno;
		printf("FAILED: cudamem_heap_block_alloc(write_buf); err(%d)\n", err);
		goto exit;
	}

	read_buf = cudamem_heap_block_alloc(&rte.cuda_heap, num_ios * BUF_SIZE);
	if (!read_buf) {
		err = errno;
		printf("FAILED: cudamem_heap_block_alloc(read_buf); err(%d)\n", err);
		goto exit;
	}

	expected = malloc(num_ios * VERIFY_SIZE);
	if (!expected) {
		err = -errno;
		printf("FAILED: malloc(expected); err(%d)\n", err);
		goto exit;
	}

	actual = malloc(num_ios * VERIFY_SIZE);
	if (!actual) {
		err = -errno;
		printf("FAILED: malloc(actual); err(%d)\n", err);
		goto exit;
	}

	// Fill each write buffer with a unique pattern based on the global IO index
	for (size_t i = 0; i < num_ios; i++) {
		for (size_t j = 0; j < VERIFY_SIZE; j++) {
			expected[i * VERIFY_SIZE + j] = (uint8_t)((i + j) % 256);
		}
	}

	memset(actual, 0, num_ios * VERIFY_SIZE);

	// Copy expected patterns into the first VERIFY_SIZE bytes of each write buffer
	for (size_t i = 0; i < num_ios; i++) {
		err = cuMemcpyHtoD((CUdeviceptr)((uint8_t *)write_buf + i * BUF_SIZE),
				   expected + i * VERIFY_SIZE, VERIFY_SIZE);
		if (err) {
			printf("FAILED: cuMemcpyHtoD(write_buf[%zu]); err(%d)\n", i, err);
			goto exit;
		}
	}

	err = prep_nvme_io(&nvme, &rte.cuda_heap, 0x1, write_buf, num_ios);
	if (err) {
		printf("FAILED: nvme_io(write); err(%d)\n", err);
		goto exit;
	}

	err = prep_nvme_io(&nvme, &rte.cuda_heap, 0x2, read_buf, num_ios);
	if (err) {
		printf("FAILED: nvme_io(read); err(%d)\n", err);
		goto exit;
	}

	// Copy back the first VERIFY_SIZE bytes of each read buffer
	for (size_t i = 0; i < num_ios; i++) {
		err = cuMemcpyDtoH(actual + i * VERIFY_SIZE,
				   (CUdeviceptr)((uint8_t *)read_buf + i * BUF_SIZE),
				   VERIFY_SIZE);
		if (err) {
			printf("FAILED: cuMemcpyDtoH(read_buf[%zu]); err(%d)\n", i, err);
			goto exit;
		}
	}

	for (size_t i = 0; i < num_ios; i++) {
		for (size_t j = 0; j < VERIFY_SIZE; j++) {
			if (expected[i * VERIFY_SIZE + j] != actual[i * VERIFY_SIZE + j]) {
				printf("FAILED: LBA %zu byte %zu: expected 0x%02x got 0x%02x\n",
				       i, j,
				       expected[i * VERIFY_SIZE + j],
				       actual[i * VERIFY_SIZE + j]);
				err = -1;
				goto exit;
			}
		}
	}

	printf("SUCCESS: %zu IOs written and read back correctly (%d queue(s), depth %d)\n",
	       num_ios, num_queues, queue_depth);

exit:
	cudamem_heap_block_free(&rte.cuda_heap, write_buf);
	cudamem_heap_block_free(&rte.cuda_heap, read_buf);
	free(expected);
	free(actual);

	nvme_term(&nvme, &rte);
	rte_term(&rte);

	return err;
}
