// SPDX-License-Identifier: BSD-3-Clause

/**
 * NVMe write and read through uio_pci_generic, with the buffers in host memory
 * ============================================================================
 *
 * The controller is opened through sysfs and DMAs against physical addresses, which is the
 * arrangement for iommu=pt or no IOMMU at all. The hugepage behind the heap is wrapped by
 * dmamem_from_hostmem_registry(), so the PRPs are physical. Writes LBA 0 of namespace 1.
 *
 * Usage:
 *   test_hostmem_nvme_readwrite <PCI-BDF>
 */

#define _UPCIE_WITH_NVME
#include <upcie/upcie.h>

#define HEAP_NBYTES (16ULL * 1024 * 1024)

struct rte {
	struct hostmem_config config;
	struct hostmem_hugepage hp;
	struct dmamem dmem;
	struct dmamem_heap heap;
	struct nvme_controller ctrlr;
	struct nvme_dmamem_uio_ctx ctx;
	struct nvme_qpair ioq;
	size_t ioq_sq, ioq_cq, ioq_prp;
	int hp_alive, dmem_alive, heap_alive, ctrlr_alive, ioq_alive;
};

static void
rte_term(struct rte *rte)
{
	if (rte->ioq_alive) {
		nvme_controller_delete_io_qpair_dmamem(&rte->ctrlr, &rte->ioq, &rte->heap,
						       rte->ioq_sq, rte->ioq_cq, rte->ioq_prp);
	}
	if (rte->ctrlr_alive) {
		nvme_controller_close_dmamem_uio(&rte->ctrlr, &rte->ctx, &rte->heap);
	}
	if (rte->heap_alive) {
		dmamem_heap_term(&rte->heap);
	}
	if (rte->dmem_alive) {
		dmamem_destroy(&rte->dmem);
	}
	if (rte->hp_alive) {
		hostmem_hugepage_free(&rte->hp);
	}
}

static int
rte_init(struct rte *rte, const char *bdf)
{
	int err;

	err = hostmem_config_init(&rte->config);
	if (err) {
		printf("FAILED: hostmem_config_init(); err(%d)\n", err);
		return err;
	}

	err = hostmem_hugepage_alloc(HEAP_NBYTES, &rte->hp, &rte->config);
	if (err) {
		printf("FAILED: hostmem_hugepage_alloc(); err(%d); are hugepages reserved?\n",
		       err);
		return err;
	}
	rte->hp_alive = 1;

	err = dmamem_from_hostmem_registry(&rte->dmem, &rte->hp, 0);
	if (err) {
		printf("FAILED: dmamem_from_hostmem_registry(); err(%d); missing CAP_SYS_ADMIN?\n",
		       err);
		return err;
	}
	rte->dmem_alive = 1;

	err = dmamem_heap_init(&rte->heap, &rte->dmem, 4096);
	if (err) {
		printf("FAILED: dmamem_heap_init(); err(%d)\n", err);
		return err;
	}
	rte->heap_alive = 1;

	nvme_dmamem_uio_ctx_init(&rte->ctx);
	err = nvme_controller_open_dmamem_uio(&rte->ctrlr, &rte->ctx, &rte->heap, bdf);
	if (err) {
		printf("FAILED: nvme_controller_open_dmamem_uio(%s); err(%d)\n", bdf, err);
		return err;
	}
	rte->ctrlr_alive = 1;

	err = nvme_controller_create_io_qpair_dmamem(&rte->ctrlr, &rte->ioq, 32, &rte->heap,
						     &rte->ioq_sq, &rte->ioq_cq, &rte->ioq_prp);
	if (err) {
		printf("FAILED: nvme_controller_create_io_qpair_dmamem(); err(%d)\n", err);
		return err;
	}
	rte->ioq_alive = 1;

	return 0;
}

static int
nvme_io(struct rte *rte, uint8_t opc, void *buffer, size_t buffer_size)
{
	struct nvme_completion cpl = {0};
	struct nvme_command cmd = {0};
	struct nvme_request *req;
	uint8_t sc, sct;
	int err;

	req = nvme_request_alloc(rte->ioq.rpool);
	if (!req) {
		printf("FAILED: nvme_request_alloc(); errno(%d)\n", errno);
		return -errno;
	}
	cmd.cid = req->cid;
	cmd.nsid = 1;
	cmd.opc = opc;
	cmd.cdw10 = 0; ///< SLBA == 0
	cmd.cdw12 = 0; ///< NLB == 0

	err = nvme_request_prep_command_prps_contig_dmamem(req, &rte->dmem, buffer, buffer_size,
							   &cmd);
	if (err) {
		printf("FAILED: nvme_request_prep_command_prps_contig_dmamem(); err(%d)\n", err);
		nvme_request_free(rte->ioq.rpool, req->cid);
		return err;
	}

	err = nvme_qpair_enqueue(&rte->ioq, &cmd);
	if (err) {
		printf("FAILED: nvme_qpair_enqueue(); err(%d)\n", err);
		nvme_request_free(rte->ioq.rpool, req->cid);
		return err;
	}

	nvme_qpair_sqdb_update(&rte->ioq);

	err = nvme_qpair_reap_cpl(&rte->ioq, rte->ctrlr.timeout_ms, &cpl);
	if (err) {
		/* Submitted and unreaped: the controller still owns this cid. */
		printf("FAILED: nvme_qpair_reap_cpl(); err(%d)\n", err);
		return err;
	}

	nvme_request_free(rte->ioq.rpool, cpl.cid);

	sc = (cpl.status & 0x1FE) >> 1;
	sct = (cpl.status & 0xE00) >> 9;
	if (sc) {
		printf("FAILED: Status Code Type(0x%x), Status Code(0x%x)\n", sct, sc);
		return -EIO;
	}

	return 0;
}

int
main(int argc, char **argv)
{
	struct rte rte = {0};
	const size_t buffer_size = 82 * sizeof(char);
	size_t write_off = 0, read_off = 0;
	char *write_buf, *read_buf;
	int err;

	if (argc != 2) {
		printf("Usage: %s <PCI-BDF>\n", argv[0]);
		return 1;
	}

	err = rte_init(&rte, argv[1]);
	if (err) {
		printf("FAILED: rte_init(); err(%d)\n", err);
		goto exit;
	}

	err = dmamem_heap_alloc(&rte.heap, buffer_size, &write_off);
	if (err) {
		printf("FAILED: dmamem_heap_alloc(write_buf); err(%d)\n", err);
		goto exit;
	}
	err = dmamem_heap_alloc(&rte.heap, buffer_size, &read_off);
	if (err) {
		printf("FAILED: dmamem_heap_alloc(read_buf); err(%d)\n", err);
		goto exit;
	}
	write_buf = dmamem_heap_at_va(&rte.heap, write_off);
	read_buf = dmamem_heap_at_va(&rte.heap, read_off);

	// Fill write buffer with ascii characters
	for (size_t i = 0; i < buffer_size; i++) {
		write_buf[i] = (i % 26) + 65;
	}

	memset(read_buf, 0, buffer_size);

	err = nvme_io(&rte, 0x1, write_buf, buffer_size);
	if (err) {
		printf("FAILED: nvme_io(write); err(%d)\n", err);
		goto exit;
	}

	err = nvme_io(&rte, 0x2, read_buf, buffer_size);
	if (err) {
		printf("FAILED: nvme_io(read); err(%d)\n", err);
		goto exit;
	}

	for (size_t i = 0; i < buffer_size; i++) {
		if (write_buf[i] != read_buf[i]) {
			printf("FAILED: written data != read data\n");
			printf("Wrote: %.*s\n", (int)buffer_size, write_buf);
			printf("Read: %.*s\n", (int)buffer_size, read_buf);
			err = -EIO;
			goto exit;
		}
	}
	printf("SUCCESS: written data == read data\n");

exit:
	rte_term(&rte);

	return err ? 1 : 0;
}
