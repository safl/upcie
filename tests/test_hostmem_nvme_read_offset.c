// SPDX-License-Identifier: BSD-3-Clause
//
// Tests that nvme_request_prep_command_prps_contig
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

#define _UPCIE_WITH_NVME
#include <upcie/upcie.h>

#define PAGESIZE 4096u
#define LBA_SIZE 512u // swissknife Samsung 990 PRO, LBA format 0

struct nvme {
	struct nvme_controller ctrlr;
	struct nvme_qpair ioq;
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

static int
read_lbas(struct nvme *nvme, void *dbuf, size_t nbytes, uint64_t slba, uint8_t *sc)
{
	struct nvme_completion cpl = {0};
	struct nvme_command cmd = {0};
	struct nvme_request *req = nvme_request_alloc(nvme->ioq.rpool);
	uint64_t nlb = (nbytes + LBA_SIZE - 1) / LBA_SIZE;
	int err;

	cmd.cid = req->cid;
	cmd.nsid = 1;
	cmd.opc = 0x2; // READ
	cmd.cdw10 = (uint32_t)(slba & 0xFFFFFFFF);
	cmd.cdw11 = (uint32_t)(slba >> 32);
	cmd.cdw12 = (uint32_t)(nlb - 1); // NLB is 0-based
	nvme_request_prep_command_prps_contig(req, nvme->ctrlr.heap, dbuf, nbytes, &cmd);

	nvme_qpair_enqueue(&nvme->ioq, &cmd);
	nvme_qpair_sqdb_update(&nvme->ioq);
	err = nvme_qpair_reap_cpl(&nvme->ioq, nvme->ctrlr.timeout_ms, &cpl);
	if (err)
		return err;
	nvme_request_free(nvme->ioq.rpool, cpl.cid);
	*sc = (cpl.status & 0x1FE) >> 1;
	return 0;
}

static int
nvme_init(struct nvme *nvme, const char *bdf, struct hostmem_heap *heap)
{
	struct nvme_completion cpl = {0};
	struct nvme_command cmd = {0};

	if (nvme_controller_open(&nvme->ctrlr, bdf, heap))
		return EIO;
	cmd.opc = 0x6; // IDENTIFY, CNS=1: brings up the controller
	cmd.cdw10 = 1;
	if (nvme_qpair_submit_sync_contig_prps(&nvme->ctrlr.aq, nvme->ctrlr.heap, nvme->ctrlr.buf,
					       4096, &cmd, nvme->ctrlr.timeout_ms, &cpl) ||
	    nvme_controller_create_io_qpair(&nvme->ctrlr, &nvme->ioq, 32)) {
		nvme_controller_close(&nvme->ctrlr);
		return EIO;
	}
	return 0;
}

int
main(int argc, char **argv)
{
	struct hostmem_config config = {0};
	struct hostmem_heap heap = {0};
	struct nvme nvme = {0};
	uint8_t *aligned, *offbuf;
	uint64_t slba = (argc == 3) ? strtoull(argv[2], NULL, 0) : 2048;
	int failures = 0, err;

	if (argc < 2 || argc > 3) {
		printf("Usage: %s <PCI-BDF> [SLBA]   (read-only)\n", argv[0]);
		return 1;
	}
	if (hostmem_config_init(&config) ||
	    hostmem_heap_init(&heap, 128ULL * 1024 * 1024, &config)) {
		printf("FAILED: heap init\n");
		return 1;
	}
	err = nvme_init(&nvme, argv[1], &heap);
	if (err) {
		printf("FAILED: nvme bring-up\n");
		hostmem_heap_term(&heap);
		return err;
	}

	// One generous allocation each; offbuf is indexed by page_off per case.
	aligned = hostmem_dma_malloc(&heap, 8u * PAGESIZE);
	offbuf = hostmem_dma_malloc(&heap, 8u * PAGESIZE);

	for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		size_t nbytes = cases[c].nbytes;
		uint8_t *dbuf = offbuf + cases[c].page_off;
		uint8_t sc_a = 0, sc_o = 0;

		memset(aligned, 0, 8u * PAGESIZE);
		memset(offbuf, 0, 8u * PAGESIZE);

		if (read_lbas(&nvme, aligned, nbytes, slba, &sc_a) || sc_a) {
			printf("[off=%4llu nbytes=%5zu] FAIL: ground-truth read sc=0x%x\n",
			       (unsigned long long)cases[c].page_off, nbytes, sc_a);
			failures++;
			continue;
		}
		if (read_lbas(&nvme, dbuf, nbytes, slba, &sc_o) || sc_o) {
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

	hostmem_dma_free(&heap, offbuf);
	hostmem_dma_free(&heap, aligned);
	nvme_controller_close(&nvme.ctrlr);
	hostmem_heap_term(&heap);
	return failures ? 1 : 0;
}
