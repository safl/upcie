// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * CPU-initiated NVMe read/write into AMD GPU memory (hipmem)
 * =========================================================
 *
 * The AMD analog of test_cudamem_nvme_readwrite: the CPU drives a stock NVMe
 * controller (host-memory queues, CPU rings the doorbell), but the data buffers
 * are GPU VRAM allocated via hipmem, so the SSD DMAs straight to/from the GPU
 * over PCIe. Write a pattern host -> GPU -> SSD, read it back SSD -> GPU -> host,
 * and compare. Needs the udmabuf-import kernel (for dmabuf_attach's physical
 * LUT) and an IOMMU in passthrough (iommu=pt) so the NVMe DMAs to the GPU's
 * physical/P2P addresses directly.
 */
#define _UPCIE_WITH_NVME
#include <upcie/upcie_hip.h>

struct rte {
	struct hostmem_config config;
	struct hostmem_heap heap;
	struct hipmem_config hip_config;
	struct hipmem_heap hip_heap;
};

struct nvme {
	struct nvme_controller ctrlr;
	struct nvme_qpair ioq;
};

int
rte_init(struct rte *rte)
{
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

	err = hipInit(0);
	if (err) {
		printf("FAILED: hipInit(); err(%d)\n", err);
		return err;
	}

	err = hipSetDevice(0);
	if (err) {
		printf("FAILED: hipSetDevice(); err(%d)\n", err);
		return err;
	}

	err = hipmem_config_init(&rte->hip_config, 0);
	if (err) {
		printf("FAILED: hipmem_config_init(); err(%d)\n", err);
		return err;
	}

	err = hipmem_heap_init(&rte->hip_heap, 1024 * 1024 * 128ULL, &rte->hip_config);
	if (err) {
		printf("FAILED: hipmem_heap_init(); err(%d)\n", err);
		return err;
	}

	return 0;
}

int
nvme_io(struct nvme *nvme, struct hipmem_heap *hip_heap, uint8_t opc, void *buffer,
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

	nvme_request_prep_command_prps_contig_hip(req, hip_heap, buffer, buffer_size, &cmd);

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
		printf("FAILED: nvme_controller_open(); err(%d)\n", err);
		return err;
	}

	cmd.opc = 0x6; ///< IDENTIFY
	cmd.cdw10 = 1; ///< CNS=1: Identify Controller

	err = nvme_qpair_submit_sync_contig_prps(&nvme->ctrlr.aq, nvme->ctrlr.heap, nvme->ctrlr.buf,
						 4096, &cmd, nvme->ctrlr.timeout_ms, &cpl);
	if (err) {
		printf("FAILED: nvme_qpair_submit_sync(); err(%d)\n", err);
		nvme_controller_close(&nvme->ctrlr);
		return err;
	}

	err = nvme_controller_create_io_qpair(&nvme->ctrlr, &nvme->ioq, 32);
	if (err) {
		printf("FAILED: nvme_controller_create_io_qpair(); err(%d)\n", err);
		return err;
	}

	return 0;
}

int
main(int argc, char **argv)
{
	struct nvme nvme = {0};
	struct rte rte = {0};
	const size_t buffer_size = 82 * sizeof(char);
	void *write_buf = NULL, *read_buf = NULL; ///< GPU IO buffers
	char *expected = NULL, *actual = NULL;    ///< host buffers for comparison
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

	write_buf = hipmem_dma_malloc(&rte.hip_heap, buffer_size);
	if (!write_buf) {
		err = errno;
		printf("FAILED: hipmem_dma_malloc(write_buf); err(%d)\n", err);
		goto exit;
	}

	read_buf = hipmem_dma_malloc(&rte.hip_heap, buffer_size);
	if (!read_buf) {
		err = errno;
		printf("FAILED: hipmem_dma_malloc(read_buf); err(%d)\n", err);
		goto exit;
	}

	expected = malloc(buffer_size);
	actual = malloc(buffer_size);
	if (!expected || !actual) {
		err = errno;
		printf("FAILED: malloc(host buffers); err(%d)\n", err);
		goto exit;
	}

	for (size_t i = 0; i < buffer_size; i++) {
		expected[i] = (i % 26) + 65;
	}
	memset(actual, 0, buffer_size);

	err = hipMemcpyHtoD((hipDeviceptr_t)write_buf, expected, buffer_size);
	if (err) {
		printf("FAILED: hipMemcpyHtoD(expected -> write_buf); err(%d)\n", err);
		goto exit;
	}

	err = hipMemcpyHtoD((hipDeviceptr_t)read_buf, actual, buffer_size);
	if (err) {
		printf("FAILED: hipMemcpyHtoD(actual -> read_buf); err(%d)\n", err);
		goto exit;
	}

	err = nvme_io(&nvme, &rte.hip_heap, 0x1, write_buf, buffer_size); ///< WRITE
	if (err) {
		printf("FAILED: nvme_io(write); err(%d)\n", err);
		goto exit;
	}

	err = nvme_io(&nvme, &rte.hip_heap, 0x2, read_buf, buffer_size); ///< READ
	if (err) {
		printf("FAILED: nvme_io(read); err(%d)\n", err);
		goto exit;
	}

	err = hipMemcpyDtoH(actual, (hipDeviceptr_t)read_buf, buffer_size);
	if (err) {
		printf("FAILED: hipMemcpyDtoH(read_buf -> actual); err(%d)\n", err);
		goto exit;
	}

	for (size_t i = 0; i < buffer_size; i++) {
		if (expected[i] != actual[i]) {
			printf("FAILED: written data != read data\n");
			goto exit;
		}
	}
	printf("PASS: NVMe round-tripped data through AMD GPU memory\n");

exit:
	hipmem_dma_free(&rte.hip_heap, write_buf);
	hipmem_dma_free(&rte.hip_heap, read_buf);
	free(expected);
	free(actual);
	nvme_controller_close(&nvme.ctrlr);
	hostmem_heap_term(&rte.heap);
	hipmem_heap_term(&rte.hip_heap);

	return err;
}
