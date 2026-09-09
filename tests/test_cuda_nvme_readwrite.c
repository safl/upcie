// SPDX-License-Identifier: BSD-3-Clause

/**
 * GPU-initiated NVMe read/write from CUDA kernels
 * ===============================================
 *
 * The controller is opened by the host through uio_pci_generic, with its admin queue on a
 * hugepage wrapped by dmamem_from_hostmem_registry(). The I/O queues are created in CUDA memory
 * with nvme_controller_cuda_create_io_qpair(), and the kernels in nvme_cuda_kernels.cu submit
 * and reap on them without the host in the loop. Needs iommu=pt, since the GPU rings doorbells
 * and the controller DMAs against physical addresses.
 *
 * Usage:
 *   test_cuda_nvme_readwrite <PCI-BDF> <num-queues> <queue-depth> <num-ios>
 */

#define _UPCIE_WITH_NVME
#include <upcie/upcie_cuda.h>

int
nvme_io_launch(struct nvme_qpair_cuda **qps, struct nvme_command *cmds, int *results,
	       uint32_t num_ios, unsigned int grid, unsigned int block);

#define BUF_SIZE (64 * 1024) ///< One CUDA heap page per IO buffer
#define VERIFY_SIZE 512      ///< Bytes to fill and verify per buffer (min NVMe sector size)
#define HEAP_NBYTES (16ULL * 1024 * 1024)

struct rte {
	struct hostmem_config config;
	struct hostmem_hugepage hp;
	struct dmamem host_dmem;
	struct dmamem_heap host_heap;
	struct cudamem_config cuda_config;
	struct cudamem_heap cuda_heap;
	struct dmamem cuda_dmem;
	CUcontext cu_ctx;
	int hp_alive, host_dmem_alive, host_heap_alive, cu_ctx_alive, cuda_heap_alive,
		cuda_dmem_alive;
};

struct nvme {
	struct nvme_controller ctrlr;
	struct nvme_dmamem_uio_ctx ctx;
	struct nvme_qpair_cuda **ioqs;    ///< Host array of device queue-pair pointers
	struct nvme_qpair_cuda **cu_ioqs; ///< Device array of queue-pair pointers
	int num_queues;
	int queue_depth;
	int ctrlr_alive;
};

void
rte_term(struct rte *rte)
{
	if (rte->cuda_dmem_alive) {
		dmamem_destroy(&rte->cuda_dmem);
	}
	if (rte->cuda_heap_alive) {
		cudamem_heap_term(&rte->cuda_heap);
	}
	if (rte->cu_ctx_alive) {
		cuCtxDestroy(rte->cu_ctx);
	}
	if (rte->host_heap_alive) {
		dmamem_heap_term(&rte->host_heap);
	}
	if (rte->host_dmem_alive) {
		dmamem_destroy(&rte->host_dmem);
	}
	if (rte->hp_alive) {
		hostmem_hugepage_free(&rte->hp);
	}
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

	err = hostmem_hugepage_alloc(HEAP_NBYTES, &rte->hp, &rte->config);
	if (err) {
		printf("FAILED: hostmem_hugepage_alloc(); err(%d)\n", err);
		return err;
	}
	rte->hp_alive = 1;

	err = dmamem_from_hostmem_registry(&rte->host_dmem, &rte->hp, 0);
	if (err) {
		printf("FAILED: dmamem_from_hostmem_registry(); err(%d)\n", err);
		return err;
	}
	rte->host_dmem_alive = 1;

	err = dmamem_heap_init(&rte->host_heap, &rte->host_dmem, 4096);
	if (err) {
		printf("FAILED: dmamem_heap_init(); err(%d)\n", err);
		return err;
	}
	rte->host_heap_alive = 1;

	err = cuInit(0);
	if (err) {
		printf("FAILED: cuInit(); err(%d)\n", err);
		return err;
	}

	err = cuDeviceGet(&cu_dev, 0);
	if (err) {
		printf("FAILED: cuDeviceGet(); err(%d)\n", err);
		return err;
	}

	err = cudamem_ctx_create(&rte->cu_ctx, cu_dev);
	if (err) {
		printf("FAILED: cuCtxCreate(); err(%d)\n", err);
		return err;
	}
	rte->cu_ctx_alive = 1;

	err = cudamem_config_init(&rte->cuda_config, 0);
	if (err) {
		printf("FAILED: cudamem_config_init(); err(%d)\n", err);
		return err;
	}

	err = cudamem_heap_init(&rte->cuda_heap, cuda_heap_size, &rte->cuda_config);
	if (err) {
		printf("FAILED: cudamem_heap_init(); err(%d)\n", err);
		return err;
	}
	rte->cuda_heap_alive = 1;

	err = dmamem_from_cuda_registry(&rte->cuda_dmem, &rte->cuda_heap, 0);
	if (err) {
		printf("FAILED: dmamem_from_cuda_registry(); err(%d)\n", err);
		return err;
	}
	rte->cuda_dmem_alive = 1;

	return 0;
}

void
nvme_term(struct nvme *nvme, struct rte *rte)
{
	for (int i = 0; i < nvme->num_queues; i++) {
		nvme_controller_cuda_delete_io_qpair(&nvme->ctrlr, nvme->ioqs[i], &rte->cuda_heap);
		cuMemFree((CUdeviceptr)nvme->ioqs[i]);
	}

	cuMemFree((CUdeviceptr)nvme->cu_ioqs);
	free(nvme->ioqs);
	if (nvme->ctrlr_alive) {
		nvme_controller_close_dmamem_uio(&nvme->ctrlr, &nvme->ctx, &rte->host_heap);
	}
}

int
nvme_init(struct nvme *nvme, const char *bdf, struct rte *rte, int num_queues, int queue_depth)
{
	int err;

	nvme->num_queues = 0;
	nvme->queue_depth = queue_depth;

	nvme_dmamem_uio_ctx_init(&nvme->ctx);
	err = nvme_controller_open_dmamem_uio(&nvme->ctrlr, &nvme->ctx, &rte->host_heap, bdf);
	if (err) {
		printf("FAILED: nvme_controller_open_dmamem_uio(); err(%d)\n", err);
		return err;
	}
	nvme->ctrlr_alive = 1;

	nvme->ioqs = calloc(num_queues, sizeof(*nvme->ioqs));
	if (!nvme->ioqs) {
		err = -errno;
		printf("FAILED: calloc(ioqs); err(%d)\n", err);
		goto err_term;
	}

	for (int i = 0; i < num_queues; i++) {
		err = cuMemAlloc((CUdeviceptr *)&nvme->ioqs[i], sizeof(struct nvme_qpair_cuda));
		if (err) {
			printf("FAILED: cuMemAlloc(ioqs[%d]); CUresult(%d)\n", i, err);
			goto err_term;
		}

		// NVMe queues hold at most depth-1 in-flight commands; add 1 so all
		// queue_depth threads can have a command outstanding simultaneously.
		err = nvme_controller_cuda_create_io_qpair(&nvme->ctrlr, nvme->ioqs[i],
							   queue_depth + 1, &rte->cuda_heap,
							   &rte->cuda_dmem);
		if (err) {
			printf("FAILED: nvme_controller_cuda_create_io_qpair(%d); err(%d)\n", i,
			       err);
			cuMemFree((CUdeviceptr)nvme->ioqs[i]);
			goto err_term;
		}

		nvme->num_queues++;
	}

	err = cuMemAlloc((CUdeviceptr *)&nvme->cu_ioqs,
			 num_queues * sizeof(struct nvme_qpair_cuda *));
	if (err) {
		printf("FAILED: cuMemAlloc(cu_ioqs); CUresult(%d)\n", err);
		goto err_term;
	}

	err = cuMemcpyHtoD((CUdeviceptr)nvme->cu_ioqs, nvme->ioqs,
			   num_queues * sizeof(struct nvme_qpair_cuda *));
	if (err) {
		printf("FAILED: cuMemcpyHtoD(cu_ioqs); CUresult(%d)\n", err);
		goto err_term;
	}

	return 0;

err_term:
	nvme_term(nvme, rte);
	return err;
}

int
prep_nvme_io(struct nvme *nvme, struct dmamem *cuda_dmem, uint8_t opc, void *buffers,
	     size_t num_ios)
{
	struct nvme_command *cmds;
	struct nvme_command *cu_cmds;
	int *results, *cu_results;
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
		cmds[gid].prp1 = dmamem_va_to_iova(cuda_dmem, (uint8_t *)buffers + gid * BUF_SIZE);
		if (!cmds[gid].prp1) {
			printf("FAILED: dmamem_va_to_iova(buffer %zu); not registered\n", gid);
			free(cmds);
			free(results);
			return -EFAULT;
		}
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

	err = nvme_io_launch(nvme->cu_ioqs, cu_cmds, cu_results, (uint32_t)num_ios,
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
	void *write_buf = NULL, *read_buf = NULL; ///< CUDA IO buffers
	uint8_t *expected = NULL, *actual = NULL; ///< HOST buffers for comparison
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
		rte_term(&rte);
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
		err = -errno;
		printf("FAILED: cudamem_heap_block_alloc(write_buf); err(%d)\n", err);
		goto exit;
	}

	read_buf = cudamem_heap_block_alloc(&rte.cuda_heap, num_ios * BUF_SIZE);
	if (!read_buf) {
		err = -errno;
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

	err = prep_nvme_io(&nvme, &rte.cuda_dmem, 0x1, write_buf, num_ios);
	if (err) {
		printf("FAILED: nvme_io(write); err(%d)\n", err);
		goto exit;
	}

	err = prep_nvme_io(&nvme, &rte.cuda_dmem, 0x2, read_buf, num_ios);
	if (err) {
		printf("FAILED: nvme_io(read); err(%d)\n", err);
		goto exit;
	}

	// Copy back the first VERIFY_SIZE bytes of each read buffer
	for (size_t i = 0; i < num_ios; i++) {
		err = cuMemcpyDtoH(actual + i * VERIFY_SIZE,
				   (CUdeviceptr)((uint8_t *)read_buf + i * BUF_SIZE), VERIFY_SIZE);
		if (err) {
			printf("FAILED: cuMemcpyDtoH(read_buf[%zu]); err(%d)\n", i, err);
			goto exit;
		}
	}

	for (size_t i = 0; i < num_ios; i++) {
		for (size_t j = 0; j < VERIFY_SIZE; j++) {
			if (expected[i * VERIFY_SIZE + j] != actual[i * VERIFY_SIZE + j]) {
				printf("FAILED: LBA %zu byte %zu: expected 0x%02x got 0x%02x\n", i,
				       j, expected[i * VERIFY_SIZE + j],
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
