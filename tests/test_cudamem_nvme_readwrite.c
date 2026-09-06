// SPDX-License-Identifier: BSD-3-Clause

/**
 * CPU-initiated NVMe read/write into NVIDIA GPU memory (cudamem)
 * ==============================================================
 *
 * The CPU drives a stock NVMe controller (host-memory queues, CPU rings the doorbell), but the
 * data buffers are GPU VRAM allocated via cudamem, so the SSD DMAs straight to/from the GPU over
 * PCIe. Write a pattern host -> GPU -> SSD, read it back SSD -> GPU -> host, and compare. Needs
 * the dmabuf-import module (for dmabuf_import_attach's physical LUT) and an IOMMU in
 * passthrough (iommu=pt) so the NVMe DMAs to the GPU's physical/P2P addresses directly.
 *
 * The controller is opened through uio_pci_generic with its queues on a hugepage wrapped by
 * dmamem_from_hostmem_registry(); the data buffers come through dmamem_from_cuda_registry().
 * Both hand the same PRP builder addresses the controller can use.
 */

#define _UPCIE_WITH_NVME
#include <upcie/upcie_cuda.h>

#define HEAP_NBYTES (16ULL * 1024 * 1024)

struct rte {
	struct hostmem_config config;
	struct hostmem_hugepage hp;
	struct dmamem host_dmem;
	struct dmamem_heap host_heap;
	struct cudamem_config cuda_config;
	struct cudamem_heap cuda_heap;
	struct dmamem dmem;
	CUcontext cu_ctx;
};

struct nvme {
	struct nvme_controller ctrlr;
	struct nvme_dmamem_uio_ctx ctx;
	struct nvme_qpair ioq;
	size_t ioq_sq, ioq_cq, ioq_prp;
	int ioq_alive;
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

	err = hostmem_hugepage_alloc(HEAP_NBYTES, &rte->hp, &rte->config);
	if (err) {
		printf("FAILED: hostmem_hugepage_alloc(); err(%d)\n", err);
		return err;
	}

	err = dmamem_from_hostmem_registry(&rte->host_dmem, &rte->hp, 0);
	if (err) {
		printf("FAILED: dmamem_from_hostmem_registry(); err(%d)\n", err);
		return err;
	}

	err = dmamem_heap_init(&rte->host_heap, &rte->host_dmem, 4096);
	if (err) {
		printf("FAILED: dmamem_heap_init(); err(%d)\n", err);
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

	err = cudamem_ctx_create(&rte->cu_ctx, cu_dev);
	if (err) {
		printf("FAILED: cuCtxCreate(); err(%d)\n", err);
		return err;
	}

	err = cudamem_config_init(&rte->cuda_config, 0);
	if (err) {
		printf("FAILED: cudamem_config_init(); err(%d)\n", err);
		return err;
	}

	err = cudamem_heap_init(&rte->cuda_heap, 1024 * 1024 * 128ULL, &rte->cuda_config);
	if (err) {
		printf("FAILED: cudamem_heap_init(); err(%d)\n", err);
		return err;
	}

	err = dmamem_from_cuda_registry(&rte->dmem, &rte->cuda_heap, 0);
	if (err) {
		printf("FAILED: dmamem_from_cuda_registry(); err(%d)\n", err);
		return err;
	}

	return 0;
}

int
nvme_io(struct nvme *nvme, struct dmamem *dmem, uint8_t opc, void *buffer, size_t buffer_size)
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

	err = nvme_request_prep_command_prps_contig_dmamem(req, dmem, buffer, buffer_size, &cmd);
	if (err) {
		printf("FAILED: prps_contig_dmamem(); err(%d)\n", err);
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
	int err;

	nvme_dmamem_uio_ctx_init(&nvme->ctx);
	err = nvme_controller_open_dmamem_uio(&nvme->ctrlr, &nvme->ctx, &rte->host_heap, bdf);
	if (err) {
		printf("FAILED: nvme_controller_open_dmamem_uio(); err(%d)\n", err);
		return err;
	}

	err = nvme_controller_create_io_qpair_dmamem(&nvme->ctrlr, &nvme->ioq, 32, &rte->host_heap,
						     &nvme->ioq_sq, &nvme->ioq_cq, &nvme->ioq_prp);
	if (err) {
		printf("FAILED: nvme_controller_create_io_qpair_dmamem(); err(%d)\n", err);
		nvme_controller_close_dmamem_uio(&nvme->ctrlr, &nvme->ctx, &rte->host_heap);
		return err;
	}
	nvme->ioq_alive = 1;

	return 0;
}

void
nvme_term(struct nvme *nvme, struct rte *rte)
{
	if (nvme->ioq_alive) {
		nvme_controller_delete_io_qpair_dmamem(&nvme->ctrlr, &nvme->ioq, &rte->host_heap,
						       nvme->ioq_sq, nvme->ioq_cq, nvme->ioq_prp);
	}
	nvme_controller_close_dmamem_uio(&nvme->ctrlr, &nvme->ctx, &rte->host_heap);
}

int
main(int argc, char **argv)
{
	struct nvme nvme = {0};
	struct rte rte = {0};
	const size_t buffer_size = 82 * sizeof(char);
	void *write_buf = NULL, *read_buf = NULL; ///< CUDA IO buffers
	char *expected = NULL, *actual = NULL;    ///< HOST buffers for comparison
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

	write_buf = cudamem_dma_malloc(&rte.cuda_heap, buffer_size);
	if (!write_buf) {
		err = errno;
		printf("FAILED: cudamem_dma_malloc(write_buf); err(%d)\n", err);
		goto exit;
	}

	read_buf = cudamem_dma_malloc(&rte.cuda_heap, buffer_size);
	if (!read_buf) {
		err = errno;
		printf("FAILED: cudamem_dma_malloc(read_buf); err(%d)\n", err);
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

	err = nvme_io(&nvme, &rte.dmem, 0x1, write_buf, buffer_size);
	if (err) {
		printf("FAILED: nvme_io(write); err(%d)\n", err);
		goto exit;
	}

	err = nvme_io(&nvme, &rte.dmem, 0x2, read_buf, buffer_size);
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
			err = EIO;
			goto exit;
		}
	}
	printf("SUCCESS: written data == read data\n");

exit:
	cudamem_dma_free(&rte.cuda_heap, write_buf);
	cudamem_dma_free(&rte.cuda_heap, read_buf);
	free(expected);
	free(actual);
	nvme_term(&nvme, &rte);
	dmamem_heap_term(&rte.host_heap);
	dmamem_destroy(&rte.host_dmem);
	hostmem_hugepage_free(&rte.hp);
	dmamem_destroy(&rte.dmem);
	cudamem_heap_term(&rte.cuda_heap);
	cuCtxDestroy(rte.cu_ctx);

	return err;
}
