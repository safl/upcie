// SPDX-License-Identifier: BSD-3-Clause

/**
 * Experimental direct NVMe<->GPU P2P DMA via kernel iommu_map
 * ============================================================
 *
 * Unlike test_cudamem_nvme_readwrite.c (which fed the importer's per-page LUT
 * straight into PRPs and failed with Data Transfer Error under vfio-pci), this
 * test asks the helper module to insert the already-known GPU device-physical
 * addresses (cuda_heap.phys_lut, obtained via the udmabuf path) directly into
 * the IOMMU domain that the VFIO-controlled NVMe device uses.
 *
 * The module maps:  IOVA_BASE + i*device_pagesize  ->  phys_lut[i]
 * so PRPs must be built from IOVAs, not from phys_lut. We do that by swapping
 * cuda_heap.phys_lut for an IOVA LUT before issuing I/O.
 *
 * IOVA_BASE must not collide with the host-mem VFIO mappings, which use
 * iova == phys (see nvme_controller_vfio.h). A high base avoids RAM/BAR ranges;
 * tune it for your platform's IOMMU aperture (VFIO_IOMMU_GET_INFO iova_ranges).
 */

#define _UPCIE_WITH_NVME
#include <upcie/upcie_cuda.h>
#include <upcie/iommu_map.h>

/*
 * IOVA window reserved for GPU pages. Constraints:
 *  - above host RAM, since host-mem VFIO maps use iova == phys (see
 *    nvme_controller_vfio.h); this box has 64 GiB RAM so stay above that.
 *  - below the IOMMU domain address width: Intel VT-d returns -EFAULT when
 *    iova+size exceeds it, and a 3-level (39-bit) domain caps IOVA at 512 GiB.
 * 256 GiB satisfies both. Check the importer's "aperture=[..]" dmesg line and
 * raise this if your platform exposes a wider aperture and needs more room.
 */
#define UPCIE_TEST_GPU_IOVA_BASE (256ULL << 30)

struct rte {
	struct hostmem_config config;
	struct hostmem_heap heap;
	struct cudamem_config cuda_config;
	struct cudamem_heap cuda_heap;
	CUcontext cu_ctx;
};

struct nvme {
	struct nvme_controller ctrlr;
	struct nvme_qpair ioq;
	struct vfio_ctx vfio;
	int importer_fd;
	uint64_t iommu_handle;
	uint64_t *phys_lut_orig;
	uint64_t *iova_lut;
};

#define UPCIE_TEST_LBA_BYTES 512u

static void
cuda_check(CUresult result, const char *what)
{
	const char *error = NULL;

	if (result == CUDA_SUCCESS)
		return;

	cuGetErrorString(result, &error);
	fprintf(stderr, "FAILED: %s: %s\n", what,
		error ? error : "unknown");
	exit(EXIT_FAILURE);
}

static int
iommu_map_cuda_heap(struct nvme *nvme, const char *bdf, struct cudamem_heap *heap)
{
	uint32_t page_size = heap->config->device_pagesize;
	int err;

	nvme->importer_fd = upcie_iommu_map_open();
	if (nvme->importer_fd < 0) {
		printf("FAILED: upcie_iommu_map_open(); err(%d)\n", nvme->importer_fd);
		return nvme->importer_fd;
	}

	/* IOVA LUT used for PRPs: iova_lut[i] = IOVA_BASE + i*device_pagesize */
	nvme->iova_lut = calloc(heap->nphys, sizeof(*nvme->iova_lut));
	if (!nvme->iova_lut) {
		err = -errno;
		printf("FAILED: calloc(iova_lut); err(%d)\n", err);
		goto error;
	}
	for (size_t i = 0; i < heap->nphys; ++i) {
		nvme->iova_lut[i] = UPCIE_TEST_GPU_IOVA_BASE + (uint64_t)i * page_size;
	}

	/* Map the true GPU device-physical addresses into NVMe's VFIO domain. */
	err = upcie_iommu_map_add(nvme->importer_fd, bdf, heap->dmabuf.fd,
					      UPCIE_TEST_GPU_IOVA_BASE, page_size,
					      heap->nphys, heap->phys_lut,
					      UPCIE_IOMMU_MAP_PROT_READ | UPCIE_IOMMU_MAP_PROT_WRITE,
					      &nvme->iommu_handle);
	if (err) {
		printf("FAILED: upcie_iommu_map_add(); err(%d)\n", err);
		goto error;
	}

	/* Build PRPs from IOVAs, not physical addresses. */
	nvme->phys_lut_orig = heap->phys_lut;
	heap->phys_lut = nvme->iova_lut;

	return 0;

error:
	free(nvme->iova_lut);
	nvme->iova_lut = NULL;
	if (nvme->importer_fd >= 0) {
		upcie_iommu_map_close(nvme->importer_fd);
		nvme->importer_fd = -1;
	}
	return err;
}

static void
iommu_unmap_cuda_heap(struct nvme *nvme, struct cudamem_heap *heap)
{
	if (nvme->phys_lut_orig) {
		heap->phys_lut = nvme->phys_lut_orig;
		nvme->phys_lut_orig = NULL;
	}

	/* Unmap from the VFIO domain BEFORE the container is torn down. */
	if (nvme->iommu_handle) {
		int err = upcie_iommu_map_del(nvme->importer_fd,
							     nvme->iommu_handle);
		if (err) {
			printf("FAILED: upcie_iommu_map_del(); err(%d)\n", err);
		}
		nvme->iommu_handle = 0;
	}

	free(nvme->iova_lut);
	nvme->iova_lut = NULL;

	if (nvme->importer_fd >= 0) {
		upcie_iommu_map_close(nvme->importer_fd);
		nvme->importer_fd = -1;
	}
}

static void
nvme_cleanup(struct nvme *nvme, struct rte *rte)
{
	if (nvme->ioq.rpool) {
		nvme_qpair_term(&nvme->ioq);
		memset(&nvme->ioq, 0, sizeof(nvme->ioq));
	}

	/* Stop the controller before removing the GPU IOVA mappings, but unmap
	 * while the VFIO container still keeps the IOMMU domain alive. */
	nvme_controller_disable_vfio(&nvme->ctrlr, &nvme->vfio);
	iommu_unmap_cuda_heap(nvme, &rte->cuda_heap);
	nvme_controller_close_vfio(&nvme->ctrlr, &nvme->vfio);
}

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

	err = cuDeviceGet(&cu_dev, 0);
	if (err) {
		printf("FAILED: cuDeviceGet(); err(%d)\n", err);
		return err;
	}

	err = cudamem_ctx_create(&rte->cu_ctx, cu_dev);
	if (err) {
		printf("FAILED: cudamem_ctx_create(); err(%d)\n", err);
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

	return 0;
}

int
nvme_io(struct nvme *nvme, uint8_t opc, uint64_t iova)
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
	cmd.cdw12 = 0; ///< NLB == 0 (1 block)
	cmd.prp1 = iova;
	cmd.prp2 = 0;

	printf("DEBUG: opc=0x%x prp1=0x%" PRIx64 "\n", opc, (uint64_t)cmd.prp1);

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
	sct = (cpl.status & 0xE00) >> 9;
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

	err = nvme_controller_open_vfio(&nvme->ctrlr, &nvme->vfio, bdf, &rte->heap);
	if (err) {
		printf("FAILED: nvme_controller_open_vfio(); err(%d)\n", err);
		return err;
	}

	err = iommu_map_cuda_heap(nvme, bdf, &rte->cuda_heap);
	if (err) {
		nvme_controller_close_vfio(&nvme->ctrlr, &nvme->vfio);
		return err;
	}

	cmd.opc = 0x6; ///< IDENTIFY
	cmd.cdw10 = 1; // CNS=1: Identify Controller

	err = nvme_qpair_submit_sync_contig_prps(&nvme->ctrlr.aq, nvme->ctrlr.heap,
						 nvme->ctrlr.buf, 4096, &cmd,
						 nvme->ctrlr.timeout_ms, &cpl);
	if (err) {
		printf("FAILED: nvme_qpair_submit_sync(); err(%d)\n", err);
		nvme_cleanup(nvme, rte);
		return err;
	}

	err = nvme_controller_create_io_qpair(&nvme->ctrlr, &nvme->ioq, 32);
	if (err) {
		printf("FAILED: nvme_controller_create_io_qpair(); err(%d)\n", err);
		nvme_cleanup(nvme, rte);
		return err;
	}

	return 0;
}

/* Dump the NVMe controller's Error Information Log (LID 0x01) for extra detail
 * about the most recent failures beyond the bare completion status code. */
static void
dump_nvme_error_log(struct nvme *nvme)
{
	struct nvme_completion cpl = {0};
	struct nvme_command cmd = {0};
	const size_t nbytes = 256;          /* 4 entries x 64 bytes */
	uint32_t numd = (nbytes / 4) - 1;   /* 0-based number of dwords */
	uint8_t *buf = (uint8_t *)nvme->ctrlr.buf;
	int err;

	cmd.opc = 0x02;                     /* Get Log Page */
	cmd.nsid = 0xFFFFFFFF;
	cmd.cdw10 = 0x01 | (numd << 16);    /* LID=Error Info, NUMDL */

	memset(buf, 0, nbytes);
	err = nvme_qpair_submit_sync_contig_prps(&nvme->ctrlr.aq, nvme->ctrlr.heap,
						 nvme->ctrlr.buf, nbytes, &cmd,
						 nvme->ctrlr.timeout_ms, &cpl);
	if (err) {
		printf("    (Get Log Page failed; err=%d)\n", err);
		return;
	}

	printf("    raw[0..31]:");
	for (int i = 0; i < 32; i++)
		printf(" %02x", buf[i]);
	printf("\n");

	for (int e = 0; e < 4; e++) {
		uint8_t *p = buf + e * 64;
		uint64_t ecount, lba, csi;
		uint16_t sqid, cid, sf, pel, tts;
		uint32_t nsid;
		uint8_t vs, trtype;

		memcpy(&ecount, p + 0, 8);
		if (ecount == 0)
			continue;
		memcpy(&sqid, p + 8, 2);
		memcpy(&cid, p + 10, 2);
		memcpy(&sf, p + 12, 2);
		memcpy(&pel, p + 14, 2);
		memcpy(&lba, p + 16, 8);
		memcpy(&nsid, p + 24, 4);
		vs = p[28];
		trtype = p[29];
		memcpy(&csi, p + 32, 8);
		memcpy(&tts, p + 40, 2);

		printf("    [errlog #%d] count=%llu sqid=%u cid=%u status=0x%04x "
		       "(SCT=0x%x SC=0x%x) param_err_loc=0x%04x\n",
		       e, (unsigned long long)ecount, sqid, cid, sf,
		       (sf & 0xE00) >> 9, (sf & 0x1FE) >> 1, pel);
		printf("                 lba=%llu nsid=0x%x vs_avail=%u trtype=%u "
		       "cmd_specific=0x%llx tts=0x%04x\n",
		       (unsigned long long)lba, nsid, vs, trtype,
		       (unsigned long long)csi, tts);
	}
}

int
main(int argc, char **argv)
{
	struct nvme nvme = {
		.importer_fd = -1,
	};
	struct rte rte = {0};
	const size_t buffer_size = UPCIE_TEST_LBA_BYTES;
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

	/*
	 * READ - WRITE - READ liveness proof:
	 *   - stage pattern A on disk (host write), READ disk->GPU, verify GPU==A
	 *   - change disk to pattern B (host write), READ disk->GPU, verify GPU==B
	 * If READ#2 returns B (not A / not the sentinel), the GPU P2P read is
	 * fetching LIVE disk content, not stale/coincidental data.
	 */
	{
		void *hostbuf = hostmem_dma_malloc(&rte.heap, buffer_size);
		uint64_t host_iova, gpu_iova;
		size_t mism;
		int fail = 0;

		if (!hostbuf) {
			err = errno;
			printf("FAILED: hostmem_dma_malloc(hostbuf); err(%d)\n", err);
			goto exit;
		}
		host_iova = hostmem_dma_v2p(&rte.heap, hostbuf);        /* iova==phys */
		gpu_iova = cudamem_heap_block_vtp(&rte.cuda_heap, read_buf);

		/* WRITE A (host -> disk) */
		for (size_t i = 0; i < buffer_size; i++)
			expected[i] = (i % 26) + 'A';
		memcpy(hostbuf, expected, buffer_size);
		printf("\n[W1] host writes pattern A to disk\n");
		err = nvme_io(&nvme, 0x1, host_iova);
		if (err) { printf("FAILED: host write A\n"); hostmem_dma_free(&rte.heap, hostbuf); goto exit; }

		/* READ #1 (disk -> GPU), expect A */
		memset(actual, 0x5A, buffer_size);
		cuda_check(cuMemcpyHtoD((CUdeviceptr)read_buf, actual,
					    buffer_size),
			     "cuMemcpyHtoD(read_buf sentinel A)");
		printf("[R1] disk -> GPU (P2P write to GPU)\n");
		err = nvme_io(&nvme, 0x2, gpu_iova);
		if (err) { printf("FAILED: GPU read #1\n"); hostmem_dma_free(&rte.heap, hostbuf); goto exit; }
		cuda_check(cuMemcpyDtoH(actual, (CUdeviceptr)read_buf,
					    buffer_size),
			     "cuMemcpyDtoH(read_buf after READ#1)");
		mism = 0;
		for (size_t i = 0; i < buffer_size; i++)
			if (actual[i] != expected[i]) mism++;
		printf("     READ#1 vs A: %zu/%zu mismatch => %s\n", mism, buffer_size, mism ? "FAIL" : "OK");
		if (mism)
			fail = 1;

		/* WRITE B (host -> disk), different pattern */
		for (size_t i = 0; i < buffer_size; i++)
			expected[i] = ((i * 7 + 3) % 26) + 'a';
		memcpy(hostbuf, expected, buffer_size);
		printf("\n[W2] host writes pattern B to disk\n");
		err = nvme_io(&nvme, 0x1, host_iova);
		if (err) { printf("FAILED: host write B\n"); hostmem_dma_free(&rte.heap, hostbuf); goto exit; }

		/* READ #2 (disk -> GPU), expect B (proves liveness) */
		memset(actual, 0xA5, buffer_size);
		cuda_check(cuMemcpyHtoD((CUdeviceptr)read_buf, actual,
					    buffer_size),
			     "cuMemcpyHtoD(read_buf sentinel B)");
		printf("[R2] disk -> GPU (P2P write to GPU)\n");
		err = nvme_io(&nvme, 0x2, gpu_iova);
		if (err) { printf("FAILED: GPU read #2\n"); hostmem_dma_free(&rte.heap, hostbuf); goto exit; }
		cuda_check(cuMemcpyDtoH(actual, (CUdeviceptr)read_buf,
					    buffer_size),
			     "cuMemcpyDtoH(read_buf after READ#2)");
		mism = 0;
		for (size_t i = 0; i < buffer_size; i++)
			if (actual[i] != expected[i]) mism++;
		printf("     READ#2 vs B: %zu/%zu mismatch => %s\n", mism, buffer_size, mism ? "FAIL" : "OK");
		if (mism)
			fail = 1;

		hostmem_dma_free(&rte.heap, hostbuf);

		printf(!fail
		       ? "\n[RESULT] R-W-R verified: storage->GPU P2P read returns LIVE disk data under VFIO\n"
		       : "\n[RESULT] storage->GPU P2P read: FAILED\n");
		err = fail ? EIO : 0;
	}

	/* [4] GPU->disk P2P read robustness: NVMe reads GPU memory (= P2P read from
	 * GPU, the direction that is platform-sensitive) over two rounds with
	 * distinct patterns. Each round verifies the data two independent ways:
	 *   (a) host read-back of the LBA (known-good path) proves the P2P read
	 *       carried the exact GPU bytes to disk;
	 *   (b) P2P write disk -> a *different* GPU buffer (pre-cleared to a
	 *       sentinel) then DtoH compare exercises the full GPU round-trip.
	 * Changing the pattern each round defeats stale-data false positives: a
	 * broken/stale P2P read would surface the previous round's data or the
	 * sentinel instead of the new pattern. */
	{
		void *hostbuf = hostmem_dma_malloc(&rte.heap, buffer_size);
		uint64_t gpu_wr_iova, gpu_rd_iova, host_iova;
		int fail = 0;

		if (!hostbuf) {
			err = errno;
			printf("FAILED: hostmem_dma_malloc(hostbuf); err(%d)\n", err);
			goto exit;
		}
		gpu_wr_iova = cudamem_heap_block_vtp(&rte.cuda_heap, write_buf);
		gpu_rd_iova = cudamem_heap_block_vtp(&rte.cuda_heap, read_buf);
		host_iova = hostmem_dma_v2p(&rte.heap, hostbuf); /* iova==phys */

		printf("\n[4] GPU->disk P2P read robustness (2 rounds, distinct patterns)\n");
		for (int it = 0; it < 2 && !fail; it++) {
			size_t hmism = 0, gmism = 0;
			int werr, rerr;

			/* distinct pattern per round */
			for (size_t i = 0; i < buffer_size; i++)
				expected[i] = ((i * (it ? 5 : 9) + (it ? 11 : 2)) % 26) + '0';
			cuda_check(cuMemcpyHtoD((CUdeviceptr)write_buf,
						    expected, buffer_size),
				     "cuMemcpyHtoD(write_buf pattern)");

			/* P2P read: NVMe reads GPU write_buf -> disk */
			printf("  [round %d] GPU->disk WRITE (P2P read from GPU)\n", it + 1);
			werr = nvme_io(&nvme, 0x1, gpu_wr_iova);
			printf("    => GPU->disk WRITE %s\n", werr ? "FAILED" : "OK");
			if (werr) {
				printf("    NVMe Error Information Log:\n");
				dump_nvme_error_log(&nvme);
				fail = 1;
				break;
			}

			/* (a) authoritative: read the LBA back via host, compare */
			memset(hostbuf, 0, buffer_size);
			rerr = nvme_io(&nvme, 0x2, host_iova); /* disk -> host */
			if (rerr) {
				printf("    => disk->host readback FAILED err(%d)\n", rerr);
				fail = 1;
				break;
			}
			for (size_t i = 0; i < buffer_size; i++)
				if (((char *)hostbuf)[i] != expected[i])
					hmism++;
			printf("    => host read-back integrity: %zu/%zu mismatch => %s\n",
			       hmism, buffer_size, hmism ? "DATA MISMATCH" : "VERIFIED");

			/* (b) full GPU round-trip: P2P write disk -> read_buf (a different
			 * GPU buffer pre-cleared to a sentinel), then DtoH compare */
			memset(actual, it ? 0xA5 : 0x5A, buffer_size);
			cuda_check(cuMemcpyHtoD((CUdeviceptr)read_buf,
						    actual, buffer_size),
				     "cuMemcpyHtoD(read_buf round-trip sentinel)");
			rerr = nvme_io(&nvme, 0x2, gpu_rd_iova); /* disk -> GPU */
			if (rerr) {
				printf("    => disk->GPU readback FAILED err(%d)\n", rerr);
				fail = 1;
				break;
			}
			cuda_check(cuMemcpyDtoH(actual, (CUdeviceptr)read_buf,
						    buffer_size),
				     "cuMemcpyDtoH(read_buf round-trip)");
			for (size_t i = 0; i < buffer_size; i++)
				if (actual[i] != expected[i])
					gmism++;
			printf("    => GPU round-trip integrity: %zu/%zu mismatch => %s\n",
			       gmism, buffer_size, gmism ? "DATA MISMATCH" : "VERIFIED");

			if (hmism || gmism)
				fail = 1;
		}
		hostmem_dma_free(&rte.heap, hostbuf);

		printf(fail
		       ? "\n[RESULT] GPU->disk P2P read: FAILED\n"
		       : "\n[RESULT] GPU->disk P2P read verified over 2 rounds (host + GPU round-trip)\n");
		if (fail && !err)
			err = EIO;
	}

exit:
	cudamem_dma_free(&rte.cuda_heap, write_buf);
	cudamem_dma_free(&rte.cuda_heap, read_buf);
	free(expected);
	free(actual);
	nvme_cleanup(&nvme, &rte);
	hostmem_heap_term(&rte.heap);
	cudamem_heap_term(&rte.cuda_heap);
	cuCtxDestroy(rte.cu_ctx);

	return err;
}
