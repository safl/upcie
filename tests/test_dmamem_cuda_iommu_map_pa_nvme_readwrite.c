// SPDX-License-Identifier: BSD-3-Clause

/**
 * NVMe <-> CUDA VRAM with an enforcing IOMMU
 * ==========================================
 *
 * The NVMe is driven through vfio-cdev + iommufd, so every address it sees is
 * an IOVA. Host memory gets there through the IOAS; CUDA memory cannot, since
 * IOMMU_IOAS_MAP_FILE rejects the dma-bufs CUDA exports, so iommu-map-pa
 * inserts it into the same domain via dmamem_from_cuda_iommu_map_pa().
 *
 * Both then compose PRPs through the same call, which is the point: one
 * command path over two very different mappings.
 *
 * test_dmamem_nvme_vram covers the neighbouring case, where the GPU is bound to
 * vfio-pci and its BAR is imported into the IOAS. Here the GPU stays with its
 * own driver and CUDA owns the memory, which is what the IOAS cannot map.
 *
 * Phase order is dictated by the kernel. The controller is opened first, since
 * the window reservation and the mappings both need a domain and the IOAS only
 * reports the ranges it will enforce once the device is attached. Teardown
 * mirrors it: mappings go before the controller closes, because detaching
 * replaces the domain they live in.
 *
 * > [!WARNING]
 * > Writes the first 4 KiB of namespace 1 on the given controller. Run it only
 * > on a device whose contents may be destroyed.
 *
 * Requires the NVMe bound to vfio-pci with an enforcing IOMMU, the
 * dmabuf-import and iommu-map-pa DKMS modules loaded, hugepages reserved, and
 * a CUDA device.
 *
 * Usage:
 *   test_dmamem_cuda_iommu_map_pa_nvme_readwrite <PCI-BDF> <vfio-cdev>
 *   e.g. ... 0000:01:00.0 /dev/vfio/devices/vfio0
 */
#define _UPCIE_WITH_NVME
#include <upcie/upcie_cuda.h>

/**
 * IOVA window handed to iommu-map-pa.
 *
 * Any range the IOAS can reach would do; 256 GiB clears host RAM and still fits
 * the 39-bit aperture Intel VT-d hands out for a 3-level domain.
 */
#define TEST_GPU_IOVA_BASE (256ULL << 30)
#define TEST_GPU_IOVA_SIZE (64ULL << 30)

#define TEST_HOSTMEM_BYTES (64ULL << 20)
#define TEST_HUGEPGSZ (2ULL << 20)
#define TEST_CUDA_HEAP_BYTES (128ULL << 20)
#define TEST_XFER_BYTES 4096u
#define TEST_NSID 1u
#define TEST_IDENTIFY_BYTES 4096u

struct rte {
	struct iommufd iommufd;
	struct dmamem host_dmem;
	struct dmamem_heap host_heap;
	struct cudamem_config cuda_config;
	struct cudamem_heap cuda_heap;
	struct dmamem gpu_dmem;
	struct dmamem_iommu_map_pa imp;
	struct nvme_controller ctrlr;
	struct nvme_dmamem_vfio_ctx ctx;
	struct nvme_qpair ioq;
	size_t ioq_sq, ioq_cq, ioq_prp;
	CUcontext cu_ctx;
	uint32_t lba_bytes; ///< Read from the namespace, not assumed
	int gpu_alive, imp_alive, ioq_alive, cuda_heap_alive;
};

static void
cuda_check(CUresult result, const char *what)
{
	const char *error = NULL;

	if (result == CUDA_SUCCESS) {
		return;
	}

	cuGetErrorString(result, &error);
	fprintf(stderr, "FAILED: %s: %s\n", what, error ? error : "unknown");
	exit(EXIT_FAILURE);
}

/** One command against `dmem`, whichever kind of memory that is. */
static int
nvme_io(struct rte *rte, struct dmamem *dmem, uint8_t opc, void *buf, size_t nbytes)
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
	cmd.nsid = TEST_NSID;
	cmd.opc = opc;
	cmd.cdw12 = (uint32_t)(nbytes / rte->lba_bytes) - 1;

	err = nvme_request_prep_command_prps_contig_dmamem(req, dmem, buf, nbytes, &cmd);
	if (err) {
		printf("FAILED: prps_contig_dmamem(%p); err(%d)\n", buf, err);
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
		/* Submitted and unreaped: the controller still owns this cid, so it
		 * cannot go back to the pool for another command to take. */
		printf("FAILED: nvme_qpair_reap_cpl(); err(%d)\n", err);
		return err;
	}
	nvme_request_free(rte->ioq.rpool, cpl.cid);

	sc = (cpl.status & 0x1FE) >> 1;
	sct = (cpl.status & 0xE00) >> 9;
	if (sc) {
		printf("FAILED: opc(0x%x) SCT(0x%x) SC(0x%x)\n", opc, sct, sc);
		return -EIO;
	}

	return 0;
}

/**
 * Round-trip one GPU buffer both ways, checking each against the other end.
 *
 * A fresh pattern per round and a sentinel in the destination, so neither stale
 * data nor a transfer that never happened can pass.
 */
static int
roundtrip(struct rte *rte, void *gpu_buf, void *host_va, int round)
{
	uint8_t expected[TEST_XFER_BYTES], actual[TEST_XFER_BYTES];
	size_t mism;
	int err;

	printf("  gpu_buf %p -> iova 0x%" PRIx64 "\n", gpu_buf,
	       dmamem_va_to_iova(&rte->gpu_dmem, gpu_buf));

	for (size_t i = 0; i < TEST_XFER_BYTES; ++i) {
		expected[i] = (uint8_t)((i * (round ? 7 : 3) + round * 29) & 0xFF);
	}

	/* GPU -> disk: the controller reads GPU memory over PCIe. */
	cuda_check(cuMemcpyHtoD((CUdeviceptr)gpu_buf, expected, TEST_XFER_BYTES),
		   "cuMemcpyHtoD(pattern)");
	err = nvme_io(rte, &rte->gpu_dmem, 0x1, gpu_buf, TEST_XFER_BYTES);
	if (err) {
		return err;
	}

	/* Check it through the host, the known-good path. */
	memset(host_va, 0, TEST_XFER_BYTES);
	err = nvme_io(rte, &rte->host_dmem, 0x2, host_va, TEST_XFER_BYTES);
	if (err) {
		return err;
	}
	mism = 0;
	for (size_t i = 0; i < TEST_XFER_BYTES; ++i) {
		mism += ((uint8_t *)host_va)[i] != expected[i];
	}
	printf("  GPU -> disk: %zu/%u bytes differ => %s\n", mism, TEST_XFER_BYTES,
	       mism ? "MISMATCH" : "VERIFIED");
	if (mism) {
		return -EIO;
	}

	/* disk -> GPU: the controller writes GPU memory over PCIe. */
	memset(actual, round ? 0xA5 : 0x5A, TEST_XFER_BYTES);
	cuda_check(cuMemcpyHtoD((CUdeviceptr)gpu_buf, actual, TEST_XFER_BYTES),
		   "cuMemcpyHtoD(sentinel)");
	err = nvme_io(rte, &rte->gpu_dmem, 0x2, gpu_buf, TEST_XFER_BYTES);
	if (err) {
		return err;
	}
	cuda_check(cuMemcpyDtoH(actual, (CUdeviceptr)gpu_buf, TEST_XFER_BYTES),
		   "cuMemcpyDtoH(readback)");
	mism = 0;
	for (size_t i = 0; i < TEST_XFER_BYTES; ++i) {
		mism += actual[i] != expected[i];
	}
	printf("  disk -> GPU: %zu/%u bytes differ => %s\n", mism, TEST_XFER_BYTES,
	       mism ? "MISMATCH" : "VERIFIED");

	return mism ? -EIO : 0;
}

/**
 * Read the namespace's LBA size, so the transfer is expressed in its blocks.
 */
static int
read_lba_bytes(struct rte *rte, uint32_t *lba_bytes)
{
	struct nvme_completion cpl = {0};
	struct nvme_command cmd = {0};
	size_t off = 0;
	uint8_t *buf;
	uint32_t lbaf;
	int err;

	err = dmamem_heap_alloc_aligned(&rte->host_heap, TEST_IDENTIFY_BYTES, 4096, &off);
	if (err) {
		printf("FAILED: dmamem_heap_alloc_aligned(identify); err(%d)\n", err);
		return err;
	}

	buf = dmamem_heap_at_va(&rte->host_heap, off);
	memset(buf, 0, TEST_IDENTIFY_BYTES);

	cmd.opc = 0x6; ///< IDENTIFY
	cmd.cid = 1;
	cmd.nsid = TEST_NSID;
	cmd.prp1 = dmamem_heap_at_iova(&rte->host_heap, off);
	cmd.cdw10 = 0x0; ///< CNS=0: Identify Namespace

	err = nvme_qpair_enqueue(&rte->ctrlr.aq, &cmd);
	if (err) {
		printf("FAILED: nvme_qpair_enqueue(identify); err(%d)\n", err);
		goto exit;
	}
	nvme_qpair_sqdb_update(&rte->ctrlr.aq);

	err = nvme_qpair_reap_cpl(&rte->ctrlr.aq, rte->ctrlr.timeout_ms, &cpl);
	if (err) {
		/* Submitted and unreaped: the controller may still write here, so it
		 * cannot go back to the heap for another allocation to take. */
		printf("FAILED: nvme_qpair_reap_cpl(identify); err(%d)\n", err);
		return err;
	}
	if ((cpl.status >> 1) & 0x7FF) {
		printf("FAILED: IDENTIFY status(0x%x)\n", cpl.status);
		err = -EIO;
		goto exit;
	}

	/* FLBAS[3:0] selects the format; LBADS is the power of two in byte 2 of it. */
	memcpy(&lbaf, &buf[128 + (buf[26] & 0xF) * 4], sizeof(lbaf));
	*lba_bytes = 1u << ((lbaf >> 16) & 0xFF);

exit:
	dmamem_heap_free(&rte->host_heap, off);

	return err;
}

static int
rte_init(struct rte *rte, const char *bdf, const char *cdev)
{
	CUdevice cu_dev;
	int err;

	if (iommufd_open(&rte->iommufd) || iommufd_ioas_alloc(&rte->iommufd)) {
		printf("FAILED: iommufd open/alloc; is an IOMMU enabled?\n");
		return -ENODEV;
	}

	err = dmamem_from_memfd(&rte->host_dmem, &rte->iommufd, TEST_HOSTMEM_BYTES, TEST_HUGEPGSZ);
	if (err) {
		printf("FAILED: dmamem_from_memfd(); err(%d); are hugepages reserved?\n", err);
		return err;
	}

	err = dmamem_heap_init(&rte->host_heap, &rte->host_dmem, 4096);
	if (err) {
		printf("FAILED: dmamem_heap_init(); err(%d)\n", err);
		return err;
	}

	if (cuInit(0) || cuDeviceGet(&cu_dev, 0) || cudamem_ctx_create(&rte->cu_ctx, cu_dev)) {
		printf("FAILED: CUDA initialisation\n");
		return -ENODEV;
	}

	err = cudamem_config_init(&rte->cuda_config, 0);
	if (err) {
		printf("FAILED: cudamem_config_init(); err(%d)\n", err);
		return err;
	}

	err = cudamem_heap_init(&rte->cuda_heap, TEST_CUDA_HEAP_BYTES, &rte->cuda_config);
	if (err) {
		printf("FAILED: cudamem_heap_init(); err(%d); is dmabuf_import loaded?\n", err);
		return err;
	}
	rte->cuda_heap_alive = 1;

	/* Attach before reserving or mapping; see the phase note up top. */
	nvme_dmamem_vfio_ctx_init(&rte->ctx);
	err = nvme_controller_open_dmamem_vfio(&rte->ctrlr, &rte->ctx, &rte->iommufd,
					       &rte->host_heap, cdev);
	if (err) {
		printf("FAILED: nvme_controller_open_dmamem_vfio(%s); err(%d)\n", cdev, err);
		return err;
	}

	err = read_lba_bytes(rte, &rte->lba_bytes);
	if (err) {
		return err;
	}
	if (TEST_XFER_BYTES % rte->lba_bytes) {
		printf("FAILED: %u-byte transfer is not a whole number of %u-byte blocks\n",
		       TEST_XFER_BYTES, rte->lba_bytes);
		return -ENOTSUP;
	}

	if (!UPCIE_HAVE_IOMMU_MAP_PA) {
		printf("FAILED: built without the iommu-map-pa UAPI\n");
		return -ENOTSUP;
	}

	err = dmamem_iommu_map_pa_open(&rte->imp, bdf, TEST_GPU_IOVA_BASE, TEST_GPU_IOVA_SIZE);
	if (err) {
		printf("FAILED: dmamem_iommu_map_pa_open(); err(%d); is iommu_map_pa loaded?\n",
		       err);
		return err;
	}
	rte->imp_alive = 1;

	err = dmamem_iommu_map_pa_reserve_window(&rte->imp, &rte->iommufd);
	if (err) {
		printf("FAILED: dmamem_iommu_map_pa_reserve_window(); err(%d)\n", err);
		return err;
	}

	err = dmamem_from_cuda_iommu_map_pa(&rte->gpu_dmem, &rte->cuda_heap, 0, &rte->imp);
	if (err) {
		printf("FAILED: dmamem_from_cuda_iommu_map_pa(); err(%d)\n", err);
		return err;
	}
	rte->gpu_alive = 1;

	err = nvme_controller_create_io_qpair_dmamem(&rte->ctrlr, &rte->ioq, 32, &rte->host_heap,
						     &rte->ioq_sq, &rte->ioq_cq, &rte->ioq_prp);
	if (err) {
		printf("FAILED: nvme_controller_create_io_qpair_dmamem(); err(%d)\n", err);
		return err;
	}
	rte->ioq_alive = 1;

	return 0;
}

static void
rte_term(struct rte *rte)
{
	if (rte->ioq_alive) {
		nvme_controller_delete_io_qpair_dmamem(&rte->ctrlr, &rte->ioq, &rte->host_heap,
						       rte->ioq_sq, rte->ioq_cq, rte->ioq_prp);
	}
	/* Unmap while the domain is still the controller's. */
	if (rte->gpu_alive) {
		dmamem_destroy(&rte->gpu_dmem);
	}
	if (rte->imp_alive) {
		dmamem_iommu_map_pa_close(&rte->imp);
	}
	if (rte->ctx.dev.fd > 0) {
		nvme_controller_close_dmamem_vfio(&rte->ctrlr, &rte->ctx, &rte->host_heap);
	}

	if (rte->cuda_heap_alive) {
		cudamem_heap_term(&rte->cuda_heap);
	}
	if (rte->cu_ctx) {
		cuCtxDestroy(rte->cu_ctx);
	}
	dmamem_heap_term(&rte->host_heap);
	dmamem_destroy(&rte->host_dmem);
	if (rte->iommufd.fd > 0) {
		iommufd_destroy(&rte->iommufd, rte->iommufd.ioas_id);
		iommufd_close(&rte->iommufd);
	}
}

int
main(int argc, char **argv)
{
	struct rte rte = {0};
	CUdeviceptr extra = 0;
	void *gpu_buf = NULL;
	size_t host_off = 0;
	void *host_va;
	int err;

	if (argc != 3) {
		printf("Usage: %s <PCI-BDF> <vfio-cdev>\n", argv[0]);
		return 1;
	}

	err = rte_init(&rte, argv[1], argv[2]);
	if (err) {
		goto exit;
	}
	printf("gpu heap 0x%" PRIx64 " mapped into the domain of %s\n", rte.cuda_heap.vaddr,
	       argv[1]);

	err = dmamem_heap_alloc_aligned(&rte.host_heap, TEST_XFER_BYTES, 4096, &host_off);
	if (err) {
		printf("FAILED: dmamem_heap_alloc_aligned(); err(%d)\n", err);
		goto exit;
	}
	host_va = dmamem_heap_at_va(&rte.host_heap, host_off);

	gpu_buf = cudamem_dma_malloc(&rte.cuda_heap, TEST_XFER_BYTES);
	if (!gpu_buf) {
		printf("FAILED: cudamem_dma_malloc(); errno(%d)\n", errno);
		err = -ENOMEM;
		goto exit;
	}

	printf("\n[1] heap-backed GPU buffer\n");
	err = roundtrip(&rte, gpu_buf, host_va, 0);
	if (err) {
		goto exit;
	}

	/* Takes its own mapping, so this also puts a second one in the window. */
	printf("\n[2] separately registered GPU allocation\n");
	cuda_check(cuMemAlloc(&extra, TEST_CUDA_HEAP_BYTES), "cuMemAlloc(extra)");
	err = dmamem_register(&rte.gpu_dmem, (void *)(uintptr_t)extra, TEST_CUDA_HEAP_BYTES);
	if (err) {
		printf("FAILED: dmamem_register(extra); err(%d)\n", err);
		goto exit;
	}
	err = roundtrip(&rte, (void *)(uintptr_t)extra, host_va, 1);
	dmamem_unregister(&rte.gpu_dmem, (void *)(uintptr_t)extra);
	if (err) {
		goto exit;
	}

	printf("\n[RESULT] NVMe DMAed to and from CUDA VRAM through the IOMMU\n");

exit:
	if (extra) {
		cuMemFree(extra);
	}
	if (gpu_buf) {
		cudamem_dma_free(&rte.cuda_heap, gpu_buf);
	}
	rte_term(&rte);

	return err ? 1 : 0;
}
