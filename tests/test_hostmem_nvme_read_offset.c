// SPDX-License-Identifier: BSD-3-Clause
//
// Tests that nvme_request_prep_command_prps_contig_dmamem
// (include/upcie/nvme/nvme_request.h) describes a buffer correctly when it
// starts at a sub-page offset within its page.
//
// Contract: reading an LBA range into a buffer at any offset must succeed and
// return the same bytes as reading the same range into a page-aligned buffer.
// Only PRP1 carries the offset; the page count and every entry past PRP1 follow
// from the page floor, so buffers whose offset pushes the transfer across a
// page boundary are the cases worth exercising.
//
// Each case reads twice, once into a page-aligned buffer (ground truth) and
// once into a buffer at the given offset, and asserts both succeed and return
// identical bytes. Read-only: the namespace is never written.
//
// The controller is opened through uio_pci_generic with the heap on a
// dmamem_from_hostmem_registry() wrap, so the PRPs are physical addresses.

#define _UPCIE_WITH_NVME
#include <upcie/upcie.h>

#define PAGESIZE 4096u
#define LBA_SIZE 512u // swissknife Samsung 990 PRO, LBA format 0
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

// page_off is the buffer's offset within its first page; nbytes must be a
// multiple of LBA_SIZE.
static const struct {
	uint64_t page_off;
	size_t nbytes;
} cases[] = {
	{0, 2u * PAGESIZE},    // aligned baseline: spans exactly two pages
	{512, PAGESIZE},       // offset pushes a one-page length onto a second page
	{512, 2u * PAGESIZE},  // offset spans three pages: PRP1, then two list entries
	{2048, 2u * PAGESIZE}, // mid-page offset spanning three pages
	{3584, 2u * PAGESIZE}, // deep offset: PRP1 carries 512 B, remainder page-aligned
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

	if (hostmem_config_init(&rte->config) ||
	    hostmem_hugepage_alloc(HEAP_NBYTES, &rte->hp, &rte->config)) {
		printf("FAILED: hugepage alloc; are hugepages reserved?\n");
		return -ENOMEM;
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
read_lbas(struct rte *rte, void *dbuf, size_t nbytes, uint64_t slba, uint8_t *sc)
{
	struct nvme_completion cpl = {0};
	struct nvme_command cmd = {0};
	struct nvme_request *req = nvme_request_alloc(rte->ioq.rpool);
	uint64_t nlb = (nbytes + LBA_SIZE - 1) / LBA_SIZE;
	int err;

	if (!req) {
		return -errno;
	}

	cmd.cid = req->cid;
	cmd.nsid = 1;
	cmd.opc = 0x2; // READ
	cmd.cdw10 = (uint32_t)(slba & 0xFFFFFFFF);
	cmd.cdw11 = (uint32_t)(slba >> 32);
	cmd.cdw12 = (uint32_t)(nlb - 1); // NLB is 0-based
	err = nvme_request_prep_command_prps_contig_dmamem(req, &rte->dmem, dbuf, nbytes, &cmd);
	if (err) {
		nvme_request_free(rte->ioq.rpool, req->cid);
		return err;
	}

	nvme_qpair_enqueue(&rte->ioq, &cmd);
	nvme_qpair_sqdb_update(&rte->ioq);
	err = nvme_qpair_reap_cpl(&rte->ioq, rte->ctrlr.timeout_ms, &cpl);
	if (err)
		return err;
	nvme_request_free(rte->ioq.rpool, cpl.cid);
	*sc = (cpl.status & 0x1FE) >> 1;
	return 0;
}

int
main(int argc, char **argv)
{
	struct rte rte = {0};
	size_t aligned_off = 0, offbuf_off = 0;
	uint8_t *aligned, *offbuf;
	uint64_t slba = (argc == 3) ? strtoull(argv[2], NULL, 0) : 2048;
	int failures = 0, err;

	if (argc < 2 || argc > 3) {
		printf("Usage: %s <PCI-BDF> [SLBA]   (read-only)\n", argv[0]);
		return 1;
	}

	err = rte_init(&rte, argv[1]);
	if (err) {
		printf("FAILED: nvme bring-up\n");
		rte_term(&rte);
		return 1;
	}

	// One generous allocation each; offbuf is indexed by page_off per case.
	if (dmamem_heap_alloc_aligned(&rte.heap, 8u * PAGESIZE, PAGESIZE, &aligned_off) ||
	    dmamem_heap_alloc_aligned(&rte.heap, 8u * PAGESIZE, PAGESIZE, &offbuf_off)) {
		printf("FAILED: dmamem_heap_alloc_aligned()\n");
		rte_term(&rte);
		return 1;
	}
	aligned = dmamem_heap_at_va(&rte.heap, aligned_off);
	offbuf = dmamem_heap_at_va(&rte.heap, offbuf_off);

	for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		size_t nbytes = cases[c].nbytes;
		uint8_t *dbuf = offbuf + cases[c].page_off;
		uint8_t sc_a = 0, sc_o = 0;

		memset(aligned, 0, 8u * PAGESIZE);
		memset(offbuf, 0, 8u * PAGESIZE);

		if (read_lbas(&rte, aligned, nbytes, slba, &sc_a) || sc_a) {
			printf("[off=%4llu nbytes=%5zu] FAIL: ground-truth read sc=0x%x\n",
			       (unsigned long long)cases[c].page_off, nbytes, sc_a);
			failures++;
			continue;
		}
		if (read_lbas(&rte, dbuf, nbytes, slba, &sc_o) || sc_o) {
			printf("[off=%4llu nbytes=%5zu] FAIL: offset read sc=0x%x\n",
			       (unsigned long long)cases[c].page_off, nbytes, sc_o);
			failures++;
			continue;
		}
		if (memcmp(aligned, dbuf, nbytes)) {
			printf("[off=%4llu nbytes=%5zu] FAIL: offset data != aligned data\n",
			       (unsigned long long)cases[c].page_off, nbytes);
			failures++;
			continue;
		}
		printf("[off=%4llu nbytes=%5zu] ok\n", (unsigned long long)cases[c].page_off,
		       nbytes);
	}

	printf("%s: %d/%zu cases passed\n", failures ? "FAILED" : "SUCCESS",
	       (int)(sizeof(cases) / sizeof(cases[0])) - failures,
	       sizeof(cases) / sizeof(cases[0]));

	dmamem_heap_free(&rte.heap, offbuf_off);
	dmamem_heap_free(&rte.heap, aligned_off);
	rte_term(&rte);
	return failures ? 1 : 0;
}
