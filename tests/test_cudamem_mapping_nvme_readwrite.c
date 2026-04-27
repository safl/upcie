// SPDX-License-Identifier: BSD-3-Clause

#define _UPCIE_WITH_NVME
#include <upcie/upcie_cuda.h>

struct rte {
	struct hostmem_config config;
	struct hostmem_heap heap;
	struct cudamem_config cuda_config;
	CUcontext cu_ctx;
};

struct nvme {
	struct nvme_controller ctrlr;
	struct nvme_qpair ioq;
};

int
rte_init(struct rte *rte)
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
		return err;
	}

	err = cuDeviceGet(&cu_dev, 0); // GPU ID 0
	if (err) {
		printf("FAILED: cuDeviceGet(); err(%d)\n", err);
		return err;
	}

	err = cuCtxCreate(&rte->cu_ctx, 0, cu_dev);
	if (err) {
		printf("FAILED: cuCtxCreate(); err(%d)\n", err);
		return err;
	}

	err = cudamem_config_init(&rte->cuda_config, 0);
	if (err) {
		printf("FAILED: cudamem_config_init(); err(%d)\n", err);
		return err;
	}

	return 0;
}

int
nvme_io(struct nvme *nvme, struct cudamem_mapping *mappings, uint8_t opc, void *buffer,
	size_t buffer_size)
{
	struct nvme_completion cpl = {0};
	struct nvme_command cmd = {0};
	struct nvme_request *req;
	uint8_t sc, sct;
	int err;

	req = nvme_request_alloc(nvme->ioq.rpool);
	if (!req) {
		err = errno;
		printf("FAILED: nvme_request_alloc(); err(%d)\n", err);
		return err;
	}
	cmd.cid = req->cid;
	cmd.nsid = 1;
	cmd.opc = opc;
	cmd.cdw10 = 0; ///< SLBA == 0
	cmd.cdw12 = 0; ///< NLB == 0

	err = nvme_request_prep_command_prps_contig_cuda_mapped(req, mappings, buffer, buffer_size,
								&cmd);
	if (err) {
		printf("FAILED: prps_contig_cuda_mapped(); err(%d)\n", err);
		return err;
	}

	err = nvme_qpair_enqueue(&nvme->ioq, &cmd);
	if (err) {
		printf("FAILED: nvme_qpair_enqueue(); err(%d)\n", err);
		return err;
	}

	nvme_qpair_sqdb_update(&nvme->ioq);

	err = nvme_qpair_reap_cpl(&nvme->ioq, nvme->ctrlr.timeout_ms, &cpl);
	if (err) {
		printf("FAILED: nvme_qpair_reap_cpl(); err(%d)\n", err);
		return err;
	}

	nvme_request_free(nvme->ioq.rpool, cpl.cid);

	sc = (cpl.status & 0x1FE) >> 1;
	sct = (cpl.status & 0xE00) >> 8;
	if (sc) {
		printf("FAILED: Status Code Type(0x%x), Status Code(0x%x)\n", sct, sc);
		err = EIO;
	}

	return err;
}

int
nvme_init(struct nvme *nvme, const char *bdf, struct rte *rte)
{
	struct nvme_completion cpl = {0};
	struct nvme_command cmd = {0};
	int err;

	err = nvme_controller_open(&nvme->ctrlr, bdf, &rte->heap);
	if (err) {
		printf("FAILED: nvme_device_open(); err(%d)\n", err);
		return err;
	}

	cmd.opc = 0x6; ///< IDENTIFY
	cmd.cdw10 = 1; // CNS=1: Identify Controller

	err = nvme_qpair_submit_sync_contig_prps(&nvme->ctrlr.aq, nvme->ctrlr.heap, nvme->ctrlr.buf,
						 4096, &cmd, nvme->ctrlr.timeout_ms, &cpl);
	if (err) {
		printf("FAILED: nvme_qpair_submit_sync(); err(%d)\n", err);
		nvme_controller_close(&nvme->ctrlr);
		return err;
	}

	err = nvme_controller_create_io_qpair(&nvme->ctrlr, &nvme->ioq, 32);
	if (err) {
		printf("FAILED: nvme_device_create_io_qpair(); err(%d)\n", err);
		return err;
	}

	return 0;
}

int
main(int argc, char **argv)
{
	struct nvme nvme = {0};
	struct rte rte = {0};
	struct cudamem_mapping *mappings = NULL;
	const size_t buffer_size = 82 * sizeof(char);
	size_t mapping_size, page;
	CUdeviceptr raw_write = 0, raw_read = 0;
	void *write_buf = NULL, *read_buf = NULL;	///< CUDA IO buffers
	char *expected = NULL, *actual = NULL;		///< HOST buffers for comparison
	int err;

	if (argc != 2) {
		printf("Usage: %s <PCI-BDF>\n", argv[0]);
		return 1;
	}

	err = rte_init(&rte);
	if (err) {
		printf("FAILED: rte_init(); err(%d)\n", err);
		return err;
	}

	err = nvme_init(&nvme, argv[1], &rte);
	if (err) {
		printf("FAILED: nvme_init(); err(%d)\n", err);
		return err;
	}

	/*
	 * cudamem_mapping requires vaddr and nbytes aligned to device_pagesize.
	 * cuMemAlloc does not guarantee that alignment, so over-allocate by one
	 * page and round the start up.
	 */
	page = rte.cuda_config.device_pagesize;
	mapping_size = page;

	err = cuMemAlloc(&raw_write, mapping_size + page);
	if (err) {
		printf("FAILED: cuMemAlloc(write); err(%d)\n", err);
		goto exit;
	}
	write_buf = (void *)(((uintptr_t)raw_write + page - 1) & ~((uintptr_t)page - 1));

	err = cuMemAlloc(&raw_read, mapping_size + page);
	if (err) {
		printf("FAILED: cuMemAlloc(read); err(%d)\n", err);
		goto exit;
	}
	read_buf = (void *)(((uintptr_t)raw_read + page - 1) & ~((uintptr_t)page - 1));

	err = cudamem_mapping_add(&mappings, write_buf, mapping_size, &rte.cuda_config, NULL);
	if (err) {
		printf("FAILED: cudamem_mapping_add(write); err(%d)\n", err);
		goto exit;
	}

	err = cudamem_mapping_add(&mappings, read_buf, mapping_size, &rte.cuda_config, NULL);
	if (err) {
		printf("FAILED: cudamem_mapping_add(read); err(%d)\n", err);
		goto exit;
	}

	expected = malloc(buffer_size);
	if (!expected) {
		err = errno;
		printf("FAILED: malloc(expected); err(%d)\n", err);
		goto exit;
	}

	actual = malloc(buffer_size);
	if (!actual) {
		err = errno;
		printf("FAILED: malloc(actual); err(%d)\n", err);
		goto exit;
	}

	// Fill buffer with ascii characters
	for (size_t i = 0; i < buffer_size; i++) {
		expected[i] = (i % 26) + 65;
	}

	memset(actual, 0, buffer_size);

	err = cuMemcpyHtoD((CUdeviceptr)write_buf, expected, buffer_size);
	if (err) {
		printf("FAILED: cuMemcpyHtoD(expected -> write_buf); err(%d)\n", err);
		goto exit;
	}

	err = cuMemcpyHtoD((CUdeviceptr)read_buf, actual, buffer_size);
	if (err) {
		printf("FAILED: cuMemcpyHtoD(actual -> read_buf); err(%d)\n", err);
		goto exit;
	}

	err = nvme_io(&nvme, mappings, 0x1, write_buf, buffer_size);
	if (err) {
		printf("FAILED: nvme_io(write); err(%d)\n", err);
		goto exit;
	}

	err = nvme_io(&nvme, mappings, 0x2, read_buf, buffer_size);
	if (err) {
		printf("FAILED: nvme_io(read); err(%d)\n", err);
		goto exit;
	}

	err = cuMemcpyDtoH(actual, (CUdeviceptr)read_buf, buffer_size);
	if (err) {
		printf("FAILED: cuMemcpyDtoH(read_buf -> actual); err(%d)\n", err);
		goto exit;
	}

	for (size_t i = 0; i < buffer_size; i++) {
		if (expected[i] != actual[i]) {
			printf("FAILED: written data != read data\n");
			printf("Wrote: %s\n", expected);
			printf("Read: %s\n", actual);
			goto exit;
		}
	}
	printf("SUCCES: written data == read data\n");

exit:
	cudamem_mapping_clear(&mappings);
	if (raw_write) {
		cuMemFree(raw_write);
	}
	if (raw_read) {
		cuMemFree(raw_read);
	}
	free(expected);
	free(actual);
	nvme_controller_close(&nvme.ctrlr);
	hostmem_heap_term(&rte.heap);
	cuCtxDestroy(rte.cu_ctx);

	return err;
}
