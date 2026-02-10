// SPDX-License-Identifier: BSD-3-Clause

#define _UPCIE_WITH_NVME
#include <upcie/upcie.h>

struct rte {
	struct hostmem_config config;
	struct hostmem_heap heap;
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

	return 0;
}

int
nvme_io(struct nvme *nvme, uint8_t opc, void *buffer, size_t buffer_size)
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

	nvme_request_prep_command_prps_contig(req, nvme->ctrlr.heap, buffer, buffer_size, &cmd);

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

	err = nvme_qpair_submit_sync_contig_prps(&nvme->ctrlr.aq, nvme->ctrlr.heap,
						 nvme->ctrlr.buf, 4096, &cmd,
						 nvme->ctrlr.timeout_ms, &cpl);
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
	const size_t buffer_size = 82 * sizeof(char);
	char *write_buf = NULL, *read_buf = NULL;
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

	write_buf = hostmem_dma_malloc(&rte.heap, buffer_size);
	if (!write_buf) {
		err = errno;
		printf("FAILED: hostmem_dma_malloc(write_buf); err(%d)\n", err);
		goto exit;
	}

	read_buf = hostmem_dma_malloc(&rte.heap, buffer_size);
	if (!read_buf) {
		err = errno;
		printf("FAILED: hostmem_dma_malloc(read_buf); err(%d)\n", err);
		goto exit;
	}

	// Fill write buffer with ascii characters
	for (size_t i = 0; i < buffer_size; i++) {
		write_buf[i] = (i % 26) + 65;
	}

	memset(read_buf, 0, buffer_size);

	err = nvme_io(&nvme, 0x1, (void *)write_buf, buffer_size);
	if (err) {
		printf("FAILED: nvme_io(write); err(%d)\n", err);
		goto exit;
	}

	err = nvme_io(&nvme, 0x2, (void *)read_buf, buffer_size);
	if (err) {
		printf("FAILED: nvme_io(read); err(%d)\n", err);
		goto exit;
	}

	for (size_t i = 0; i < buffer_size; i++) {
		if (write_buf[i] != read_buf[i]) {
			printf("FAILED: written data != read data\n");
			printf("Wrote: %s\n", write_buf);
			printf("Read: %s\n", read_buf);
			goto exit;
		}
	}
	printf("SUCCES: written data == read data\n");

exit:
	hostmem_dma_free(&rte.heap, write_buf);
	hostmem_dma_free(&rte.heap, read_buf);
	nvme_controller_close(&nvme.ctrlr);
	hostmem_heap_term(&rte.heap);

	return err;
}