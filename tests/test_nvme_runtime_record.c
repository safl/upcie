// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * Export a runtime and use it back, in one process
 * ===============================================
 *
 * A client of a served controller builds its own struct from a record and
 * a allocation, rather than receiving a pointer to somebody else's. That
 * construction is worth testing before any of it crosses a process boundary,
 * because a fault in it looks exactly like a fault in the control plane.
 *
 * So: open a controller, create an I/O queue pair, put the record in the heap
 * where a client can find it, then attach to that heap by descriptor and
 * build a second controller and queue pair from what is in it, and read LBA 0
 * through the second one. Same process, no sockets, and the attached side
 * takes its physical addresses from the heap header rather than from pagemap,
 * which is what lets an unprivileged client translate at all.
 *
 * Usage:
 *   test_nvme_runtime_record <bdf> [vfio]
 *   e.g.
 *   test_nvme_runtime_record 0000:c4:00.0
 *   test_nvme_runtime_record 0000:c4:00.0 vfio
 */
#define _UPCIE_WITH_NVME
#include <upcie/upcie.h>

#define NBYTES 4096

int
main(int argc, char *argv[])
{
	struct nvme_runtime_record *record;
	struct hostmem_heap attached_heap = {0};
	struct nvme_ioqpair allocation = {0};
	struct nvme_controller ctrlr = {0};
	struct nvme_controller imported = {0};
	struct hostmem_config config = {0};
	struct hostmem_heap heap = {0};
	struct nvme_qpair qpair = {0};
	struct nvme_qpair attached = {0};
	struct nvme_command cmd = {0};
	struct nvme_completion cpl = {0};
	struct vfio_ctx vfio = {0};
	void *payload;
	void *prps;
	int use_vfio;
	int err;

	if ((argc < 2) || (argc > 3) || ((argc == 3) && strcmp(argv[2], "vfio"))) {
		fprintf(stderr, "usage: %s <bdf> [vfio]\n", argv[0]);
		return 2;
	}
	use_vfio = (argc == 3);

	hostmem_config_init(&config);

	err = hostmem_heap_init(&heap, 64 * 1024 * 1024, &config);
	if (err) {
		printf("# FAILED: hostmem_heap_init(); err(%d)\n", err);
		return 1;
	}

	/* Both map the same heap; what differs is whether the device is reached
	 * through sysfs with addresses from pagemap, or through vfio with the
	 * heap mapped into an IOMMU domain. */
	err = use_vfio ? nvme_controller_open_vfio(&ctrlr, &vfio, argv[1], &heap)
		       : nvme_controller_open(&ctrlr, argv[1], &heap);
	if (err) {
		printf("# FAILED: nvme_controller_open%s(); err(%d)\n", use_vfio ? "_vfio" : "",
		       err);
		return 1;
	}

	err = nvme_controller_create_io_qpair(&ctrlr, &qpair, 64);
	if (err) {
		printf("# FAILED: nvme_controller_create_io_qpair(); err(%d)\n", err);
		return 1;
	}

	prps = hostmem_dma_alloc_array(&heap, NVME_REQUEST_POOL_LEN, config.pagesize);
	if (!prps) {
		printf("# FAILED: hostmem_dma_alloc_array(prps); errno(%d)\n", errno);
		return 1;
	}

	record = hostmem_dma_malloc(&heap, sizeof(*record));
	if (!record) {
		printf("# FAILED: hostmem_dma_malloc(record); errno(%d)\n", errno);
		return 1;
	}

	err = nvme_runtime_record_export(&ctrlr, heap.memory.size, record);
	if (!err) {
		err = nvme_ioqpair_export(&ctrlr, &qpair, prps, &allocation);
	}
	if (err) {
		printf("# FAILED: export; err(%d)\n", err);
		return 1;
	}

	hostmem_heap_record_set(&heap, (uint64_t)((char *)record - (char *)heap.memory.virt));
	printf("record: version(%u) bdf(%s) timeout_ms(%u) at heap offset 0x%" PRIx64 "\n",
	       record->version, record->bdf, record->timeout_ms, hostmem_heap_record_get(&heap));
	printf("allocation:  qid(%u) depth(%u) sq_offset(0x%" PRIx64 ") cq_offset(0x%" PRIx64
	       ")\n",
	       allocation.qid, allocation.depth, allocation.sq_offset, allocation.cq_offset);

	/* Everything below this line pretends to be another process: it has the
	 * heap descriptor, the allocation, and a mapping of BAR0, and finds the
	 * record for itself. */
	err = hostmem_heap_attach(&attached_heap, hostmem_heap_fd(&heap), &config);
	if (err) {
		printf("# FAILED: hostmem_heap_attach(); err(%d)\n", err);
		return 1;
	}
	if (!hostmem_heap_record_get(&attached_heap)) {
		printf("# FAILED: attached heap carries no record offset\n");
		return 1;
	}
	printf("# LGTM: attached by descriptor, %u hugepages, phys[0]=0x%" PRIx64 "\n",
	       (unsigned)attached_heap.nphys, attached_heap.phys_lut[0]);

	err = nvme_runtime_record_import(
		&imported,
		(const struct nvme_runtime_record *)((char *)attached_heap.memory.virt +
						     hostmem_heap_record_get(&attached_heap)),
		ctrlr.func.bars[0].region, &attached_heap);
	if (err) {
		printf("# FAILED: nvme_runtime_record_import(); err(%d)\n", err);
		return 1;
	}

	err = nvme_ioqpair_import(&attached, &allocation, &imported);
	if (err) {
		printf("# FAILED: nvme_ioqpair_import(); err(%d)\n", err);
		return 1;
	}

	if (attached.sqdb != qpair.sqdb || attached.cqdb != qpair.cqdb) {
		printf("# FAILED: imported queue rings a different doorbell\n");
		return 1;
	}
	if (hostmem_dma_v2p(&attached_heap, attached.sq) != hostmem_dma_v2p(&heap, qpair.sq)) {
		printf("# FAILED: imported queue resolves to different memory\n");
		return 1;
	}
	printf("# LGTM: imported queue resolves to the same doorbell and memory\n");

	payload = hostmem_dma_malloc(&heap, NBYTES); ///< The server allocates; see the design
	if (!payload) {
		printf("# FAILED: hostmem_dma_malloc(); errno(%d)\n", errno);
		return 1;
	}
	memset(payload, 0, NBYTES);

	cmd.opc = 0x2; ///< Read
	cmd.nsid = 1;
	cmd.prp1 = hostmem_dma_v2p(&attached_heap, payload);
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

	nvme_ioqpair_release(&attached);
	hostmem_heap_detach(&attached_heap);
	hostmem_dma_free(&heap, payload);
	nvme_controller_delete_io_qpair(&ctrlr, &qpair);
	if (use_vfio) {
		nvme_controller_close_vfio(&ctrlr, &vfio);
	} else {
		nvme_controller_close(&ctrlr);
	}
	hostmem_heap_term(&heap);

	return 0;
}
