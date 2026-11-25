// SPDX-License-Identifier: BSD-3-Clause

/**
 * NVMe read/write on HIP VRAM through an IOMMU, with iommu_map_pa
 * ===============================================================
 *
 * The HIP counterpart of test_dmamem_cuda_iommu_map_pa_nvme_readwrite. The NVMe
 * controller is opened through a vfio cdev and attached to an iommufd IOAS, so
 * the controller addresses memory by IOVA. Host memory is mapped through the
 * IOAS. HIP memory is not, because IOMMU_IOAS_MAP_FILE fails for the dma-bufs
 * that amdgpu exports; the iommu_map_pa module maps it into the controller's
 * domain instead, through dmamem_from_hip_iommu_map_pa(). Commands on both kinds
 * of memory are prepared by nvme_request_prep_command_prps_contig_dmamem().
 *
 * The test round-trips 4 KiB between VRAM and LBA 0 for a buffer from the HIP
 * heap, and again for a hipMalloc() allocation registered with
 * dmamem_register().
 *
 * test_dmamem_nvme_vram covers a different case: the GPU is bound to vfio-pci
 * and its BAR is imported into the IOAS. In this test the GPU stays bound to
 * amdgpu and the HIP runtime allocates the memory.
 *
 * The controller is opened before the IOVA window is reserved and before
 * anything is mapped, because the IOAS reports the IOVA ranges it allows only
 * once a device is attached, and iommu_map_pa maps into the domain the
 * controller is attached to. The mappings are removed before the controller is
 * closed, because detaching replaces that domain.
 *
 * WARNING: the test writes the first 4 KiB of namespace 1 on the given
 * controller.
 *
 * Requires the controller bound to vfio-pci behind a translating IOMMU, the
 * dmabuf_import and iommu_map_pa modules loaded, 2 MiB hugepages reserved, and
 * a HIP device.
 *
 * Usage:
 *   test_dmamem_hip_iommu_map_pa_nvme_readwrite <PCI-BDF> <vfio-cdev>
 *   e.g. test_dmamem_hip_iommu_map_pa_nvme_readwrite 0000:01:00.0 /dev/vfio/devices/vfio0
 */
#define _UPCIE_WITH_NVME
#include <upcie/upcie_hip.h>

/**
 * The IOVA window that dmamem_iommu_map_pa_open() allocates from
 *
 * Any range inside the aperture of the IOAS works. [256 GiB, 320 GiB) lies
 * inside the 39-bit aperture of an Intel VT-d domain with 3-level page tables.
 */
#define TEST_GPU_IOVA_BASE (256ULL << 30)
#define TEST_GPU_IOVA_SIZE (64ULL << 30)

#define TEST_HOSTMEM_BYTES (64ULL << 20)
#define TEST_HUGEPGSZ (2ULL << 20)
#define TEST_HIP_HEAP_BYTES (128ULL << 20)
#define TEST_XFER_BYTES 4096u
#define TEST_NSID 1u
#define TEST_IDENTIFY_BYTES 4096u

struct rte {
	struct iommufd iommufd;
	struct dmamem host_dmem;
	struct dmamem_heap host_heap;
	struct hipmem_config hip_config;
	struct hipmem_heap hip_heap;
	struct dmamem gpu_dmem;
	struct dmamem_iommu_map_pa imp;
	struct nvme_controller ctrlr;
	struct nvme_dmamem_vfio_ctx ctx;
	struct nvme_qpair ioq;
	size_t ioq_sq, ioq_cq, ioq_prp;
	uint32_t lba_bytes; ///< LBA size of namespace TEST_NSID, from Identify Namespace
	int gpu_alive, imp_alive, ioq_alive, hip_heap_alive;
};

static void
hip_check(hipError_t result, const char *what)
{
	if (result == hipSuccess) {
		return;
	}

	fprintf(stderr, "FAILED: %s: %s\n", what, hipGetErrorString(result));
	exit(EXIT_FAILURE);
}

/** Submit one command whose data is `nbytes` at `buf` in `dmem`, and wait for its completion. */
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

	err = nvme_qpair_reap_cpl(&rte->ioq, rte->ctrlr.base.timeout_ms, &cpl);
	if (err) {
		/* The request is not freed: the command may still complete, and a
		 * later command must not reuse its cid. */
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
 * Write `gpu_buf` to LBA 0 and read LBA 0 back into `gpu_buf`, verifying both
 *
 * The pattern differs per `round`, and `gpu_buf` is overwritten with a sentinel
 * before the read, so data left by an earlier round, or a read that transferred
 * nothing, fails the comparison.
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
	hip_check(hipMemcpyHtoD((hipDeviceptr_t)gpu_buf, expected, TEST_XFER_BYTES),
		  "hipMemcpyHtoD(pattern)");
	err = nvme_io(rte, &rte->gpu_dmem, 0x1, gpu_buf, TEST_XFER_BYTES);
	if (err) {
		return err;
	}

	/* Verify the write by reading LBA 0 into host memory. */
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
	hip_check(hipMemcpyHtoD((hipDeviceptr_t)gpu_buf, actual, TEST_XFER_BYTES),
		  "hipMemcpyHtoD(sentinel)");
	err = nvme_io(rte, &rte->gpu_dmem, 0x2, gpu_buf, TEST_XFER_BYTES);
	if (err) {
		return err;
	}
	hip_check(hipMemcpyDtoH(actual, (hipDeviceptr_t)gpu_buf, TEST_XFER_BYTES),
		  "hipMemcpyDtoH(readback)");
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
	uint32_t lbaf, lbads;
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

	err = nvme_qpair_reap_cpl(&rte->ctrlr.aq, rte->ctrlr.base.admin_timeout_ms, &cpl);
	if (err) {
		/* The buffer is not freed: the controller may still write the
		 * Identify data to it. */
		printf("FAILED: nvme_qpair_reap_cpl(identify); err(%d)\n", err);
		return err;
	}
	if ((cpl.status >> 1) & 0x7FF) {
		printf("FAILED: IDENTIFY status(0x%x)\n", cpl.status);
		err = -EIO;
		goto exit;
	}

	/* FLBAS[3:0] indexes the LBA format table at byte 128. LBADS, byte 2 of
	 * a 4-byte entry, is log2 of the LBA size. */
	memcpy(&lbaf, &buf[128 + (buf[26] & 0xF) * 4], sizeof(lbaf));
	lbads = (lbaf >> 16) & 0xFF;
	if (lbads < 9 || lbads > 12) {
		printf("FAILED: LBADS(%u) outside 9..12\n", lbads);
		err = -ENOTSUP;
		goto exit;
	}
	*lba_bytes = 1u << lbads;

exit:
	dmamem_heap_free(&rte->host_heap, off);

	return err;
}

static int
rte_init(struct rte *rte, const char *bdf, const char *cdev)
{
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

	if (hipInit(0) != hipSuccess || hipSetDevice(0) != hipSuccess) {
		printf("FAILED: HIP initialisation\n");
		return -ENODEV;
	}

	err = hipmem_config_init(&rte->hip_config, 0);
	if (err) {
		printf("FAILED: hipmem_config_init(); err(%d)\n", err);
		return err;
	}

	err = hipmem_heap_init(&rte->hip_heap, TEST_HIP_HEAP_BYTES, &rte->hip_config);
	if (err) {
		printf("FAILED: hipmem_heap_init(); err(%d); is dmabuf_import loaded?\n", err);
		return err;
	}
	rte->hip_heap_alive = 1;

	/* Opened before the window is reserved and the heap is mapped; see the file comment. */
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

	err = dmamem_from_hip_iommu_map_pa(&rte->gpu_dmem, &rte->hip_heap, 0, &rte->imp);
	if (err) {
		printf("FAILED: dmamem_from_hip_iommu_map_pa(); err(%d)\n", err);
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
	/* dmamem_destroy() unmaps from the domain that closing the controller replaces. */
	if (rte->gpu_alive) {
		dmamem_destroy(&rte->gpu_dmem);
	}
	if (rte->imp_alive) {
		dmamem_iommu_map_pa_close(&rte->imp);
	}
	if (rte->ctx.dev.fd > 0) {
		nvme_controller_close_dmamem_vfio(&rte->ctrlr, &rte->ctx, &rte->host_heap);
	}

	if (rte->hip_heap_alive) {
		hipmem_heap_term(&rte->hip_heap);
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
	void *extra = NULL;
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
	printf("gpu heap 0x%" PRIx64 " mapped into the domain of %s\n", rte.hip_heap.vaddr,
	       argv[1]);

	err = dmamem_heap_alloc_aligned(&rte.host_heap, TEST_XFER_BYTES, 4096, &host_off);
	if (err) {
		printf("FAILED: dmamem_heap_alloc_aligned(); err(%d)\n", err);
		goto exit;
	}
	host_va = dmamem_heap_at_va(&rte.host_heap, host_off);

	gpu_buf = hipmem_dma_malloc(&rte.hip_heap, TEST_XFER_BYTES);
	if (!gpu_buf) {
		printf("FAILED: hipmem_dma_malloc(); errno(%d)\n", errno);
		err = -ENOMEM;
		goto exit;
	}

	printf("\n[1] heap-backed GPU buffer\n");
	err = roundtrip(&rte, gpu_buf, host_va, 0);
	if (err) {
		goto exit;
	}

	/* dmamem_register() installs a second mapping in the IOVA window, beside the heap's. */
	printf("\n[2] separately registered GPU allocation\n");
	hip_check(hipMalloc(&extra, TEST_HIP_HEAP_BYTES), "hipMalloc(extra)");
	err = dmamem_register(&rte.gpu_dmem, extra, TEST_HIP_HEAP_BYTES);
	if (err) {
		printf("FAILED: dmamem_register(extra); err(%d)\n", err);
		goto exit;
	}
	err = roundtrip(&rte, extra, host_va, 1);
	dmamem_unregister(&rte.gpu_dmem, extra);
	if (err) {
		goto exit;
	}

	printf("\n[RESULT] NVMe DMAed to and from HIP VRAM through the IOMMU\n");

exit:
	if (extra) {
		hipFree(extra);
	}
	if (gpu_buf) {
		hipmem_dma_free(&rte.hip_heap, gpu_buf);
	}
	rte_term(&rte);

	return err ? 1 : 0;
}
