// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * Export a runtime and use it back, in one process
 * ===============================================
 *
 * A consumer of a delegated controller builds its own struct from a record and
 * a grant, rather than receiving a pointer to somebody else's. That
 * construction is worth testing before any of it crosses a process boundary,
 * because a fault in it looks exactly like a fault in the delegation.
 *
 * So: open a controller, create an I/O queue pair, describe both, then build a
 * second controller and queue pair from the description alone and read LBA 0
 * through the second one. Same process, no sockets, no privilege beyond what
 * opening the device already took.
 *
 * Usage:
 *   test_nvme_runtime_record <bdf>
 *   e.g.
 *   test_nvme_runtime_record 0000:c4:00.0
 */
#define _UPCIE_WITH_NVME
#include <upcie/upcie.h>

#define NBYTES 4096

int
main(int argc, char *argv[])
{
	struct nvme_runtime_record record = {0};
	struct nvme_qpair_grant grant = {0};
	struct nvme_controller ctrlr = {0};
	struct nvme_controller imported = {0};
	struct hostmem_config config = {0};
	struct hostmem_heap heap = {0};
	struct nvme_qpair qpair = {0};
	struct nvme_qpair attached = {0};
	struct nvme_command cmd = {0};
	struct nvme_completion cpl = {0};
	void *payload;
	int err;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <bdf>\n", argv[0]);
		return 2;
	}

	hostmem_config_init(&config);

	err = hostmem_heap_init(&heap, 64 * 1024 * 1024, &config);
	if (err) {
		printf("# FAILED: hostmem_heap_init(); err(%d)\n", err);
		return 1;
	}

	err = nvme_controller_open(&ctrlr, argv[1], &heap);
	if (err) {
		printf("# FAILED: nvme_controller_open(); err(%d)\n", err);
		return 1;
	}

	err = nvme_controller_create_io_qpair(&ctrlr, &qpair, 64);
	if (err) {
		printf("# FAILED: nvme_controller_create_io_qpair(); err(%d)\n", err);
		return 1;
	}

	err = nvme_runtime_record_export(&ctrlr, &record);
	if (!err) {
		err = nvme_qpair_grant_export(&ctrlr, &qpair, &grant);
	}
	if (err) {
		printf("# FAILED: export; err(%d)\n", err);
		return 1;
	}
	printf("record: version(%u) bdf(%s) timeout_ms(%u)\n", record.version, record.bdf,
	       record.timeout_ms);
	printf("grant:  qid(%u) depth(%u) sq_offset(0x%" PRIx64 ") cq_offset(0x%" PRIx64 ")\n",
	       grant.qid, grant.depth, grant.sq_offset, grant.cq_offset);

	/* Everything below this line pretends to be another process: it has the
	 * record, the grant, a mapping of the heap and a mapping of BAR0, and
	 * nothing else. */
	err = nvme_runtime_record_import(&imported, &record, ctrlr.func.bars[0].region, &heap);
	if (err) {
		printf("# FAILED: nvme_runtime_record_import(); err(%d)\n", err);
		return 1;
	}

	err = nvme_qpair_grant_import(&attached, &grant, &imported);
	if (err) {
		printf("# FAILED: nvme_qpair_grant_import(); err(%d)\n", err);
		return 1;
	}

	if ((attached.sqdb != qpair.sqdb) || (attached.cqdb != qpair.cqdb) ||
	    (attached.sq != qpair.sq) || (attached.cq != qpair.cq)) {
		printf("# FAILED: imported queue does not describe the same memory\n");
		return 1;
	}
	printf("# LGTM: imported queue resolves to the same addresses\n");

	payload = hostmem_dma_malloc(&heap, NBYTES);
	if (!payload) {
		printf("# FAILED: hostmem_dma_malloc(); errno(%d)\n", errno);
		return 1;
	}
	memset(payload, 0, NBYTES);

	cmd.opc = 0x2; ///< Read
	cmd.nsid = 1;
	cmd.prp1 = hostmem_dma_v2p(&heap, payload);
	cmd.cdw10 = 0; ///< SLBA low
	cmd.cdw12 = 0; ///< Read one block

	err = nvme_qpair_submit_sync(&attached, &cmd, imported.timeout_ms, &cpl);
	if (err) {
		printf("# FAILED: nvme_qpair_submit_sync(); err(%d)\n", err);
		return 1;
	}
	{
		uint8_t sc = (cpl.status & 0x1FE) >> 1;
		uint8_t sct = (cpl.status & 0xE00) >> 8;

		if (sc || sct) {
			printf("# FAILED: read; sct(0x%x) sc(0x%x)\n", sct, sc);
			return 1;
		}
	}

	printf("# LGTM: read LBA 0 through the imported queue\n");

	nvme_qpair_grant_release(&attached);
	hostmem_dma_free(&heap, payload);
	nvme_controller_delete_io_qpair(&ctrlr, &qpair);
	nvme_controller_close(&ctrlr);
	hostmem_heap_term(&heap);

	return 0;
}
