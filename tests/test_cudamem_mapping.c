// SPDX-License-Identifier: BSD-3-Clause

#include <upcie/upcie_cuda.h>
#include <cuda.h>

#define EXPECT_EQ(label, got, want)                                                      \
	do {                                                                             \
		long long _g = (long long)(got);                                         \
		long long _w = (long long)(want);                                        \
		if (_g != _w) {                                                          \
			printf("# FAILED: %s; got(%lld) want(%lld)\n", (label), _g, _w); \
			return 1;                                                        \
		}                                                                        \
	} while (0)

static int
test_add_remove_clear(struct cudamem_mapping_registry *registry, struct cudamem_config *config)
{
	const size_t page = (size_t)config->device_pagesize;
	const size_t nbytes = 4 * page;
	CUdeviceptr raw = 0;
	void *aligned = NULL;
	size_t aligned_nbytes = 0;
	uint64_t phys = 0;
	int err;

	printf("# test_add_remove_clear\n");

	err = (cuMemAlloc(&raw, nbytes) == CUDA_SUCCESS) ? 0 : -ENOMEM;
	EXPECT_EQ("cuMemAlloc", err, 0);
	aligned = (void *)raw;
	aligned_nbytes = nbytes;

	err = cudamem_mapping_add(registry, config, aligned, aligned_nbytes, NULL);
	EXPECT_EQ("cudamem_mapping_add", err, 0);

	err = cudamem_mapping_virt_to_phys(registry, aligned, &phys);
	EXPECT_EQ("virt_to_phys(base)", err, 0);
	if (phys == 0) {
		printf("# FAILED: phys is zero\n");
		return 1;
	}

	err = cudamem_mapping_remove(registry, aligned);
	EXPECT_EQ("cudamem_mapping_remove", err, 0);

	err = cudamem_mapping_virt_to_phys(registry, aligned, &phys);
	EXPECT_EQ("virt_to_phys(after remove)", err, -EINVAL);

	err = cudamem_mapping_add(registry, config, aligned, aligned_nbytes, NULL);
	EXPECT_EQ("cudamem_mapping_add(re-add)", err, 0);

	cudamem_mapping_clear(registry);
	if (registry->list != NULL) {
		printf("# FAILED: registry->list non-NULL after clear\n");
		return 1;
	}

	cuMemFree(raw);
	return 0;
}

static int
test_unaligned_lookup(struct cudamem_mapping_registry *registry, struct cudamem_config *config)
{
	const size_t page = (size_t)config->device_pagesize;
	const size_t nbytes = 4 * page;
	CUdeviceptr raw = 0;
	void *aligned = NULL;
	size_t aligned_nbytes = 0;
	uint64_t base_phys = 0, off_phys = 0;
	const uint64_t in_page_offset = 1234;
	const uint64_t cross_page_offset = page + 4321;
	int err;

	printf("# test_unaligned_lookup\n");

	err = (cuMemAlloc(&raw, nbytes) == CUDA_SUCCESS) ? 0 : -ENOMEM;
	EXPECT_EQ("cuMemAlloc", err, 0);
	aligned = (void *)raw;
	aligned_nbytes = nbytes;

	err = cudamem_mapping_add(registry, config, aligned, aligned_nbytes, NULL);
	EXPECT_EQ("cudamem_mapping_add", err, 0);

	err = cudamem_mapping_virt_to_phys(registry, aligned, &base_phys);
	EXPECT_EQ("virt_to_phys(base)", err, 0);

	/* Unaligned VA inside the first page must resolve to base_phys + offset. */
	err = cudamem_mapping_virt_to_phys(registry, (uint8_t *)aligned + in_page_offset,
					   &off_phys);
	EXPECT_EQ("virt_to_phys(in-page offset)", err, 0);
	if (off_phys != base_phys + in_page_offset) {
		printf("# FAILED: in-page offset mismatch; got(0x%" PRIx64 ") want(0x%" PRIx64
		       ")\n",
		       off_phys, base_phys + in_page_offset);
		return 1;
	}

	/* Unaligned VA in a later page resolves through a different LUT slot;
	 * within-page offset must still be added. */
	uint64_t page2_base = 0;
	err = cudamem_mapping_virt_to_phys(registry, (uint8_t *)aligned + page, &page2_base);
	EXPECT_EQ("virt_to_phys(page2 base)", err, 0);

	err = cudamem_mapping_virt_to_phys(registry, (uint8_t *)aligned + cross_page_offset,
					   &off_phys);
	EXPECT_EQ("virt_to_phys(cross-page offset)", err, 0);
	if (off_phys != page2_base + (cross_page_offset - page)) {
		printf("# FAILED: cross-page offset mismatch; got(0x%" PRIx64 ") want(0x%" PRIx64
		       ")\n",
		       off_phys, page2_base + (cross_page_offset - page));
		return 1;
	}

	cudamem_mapping_clear(registry);
	cuMemFree(raw);
	return 0;
}

static int
test_arbitrary_alignment(struct cudamem_mapping_registry *registry, struct cudamem_config *config)
{
	const size_t host_page = (size_t)config->pagesize;
	const size_t dev_page = (size_t)config->device_pagesize;
	const size_t nbytes = 4 * dev_page;
	CUdeviceptr raw = 0;
	void *aligned = NULL;
	size_t aligned_nbytes = 0;
	int err;

	printf("# test_arbitrary_alignment\n");

	err = (cuMemAlloc(&raw, nbytes) == CUDA_SUCCESS) ? 0 : -ENOMEM;
	EXPECT_EQ("cuMemAlloc", err, 0);
	aligned = (void *)raw;
	aligned_nbytes = nbytes;

	/* Sub-host-page vaddr: accepted (chunk cache resolves at byte granularity). */
	err = cudamem_mapping_add(registry, config, (uint8_t *)aligned + 1, aligned_nbytes, NULL);
	EXPECT_EQ("add(sub-host-page vaddr)", err, 0);
	cudamem_mapping_clear(registry);

	/* Sub-host-page size: accepted. */
	err = cudamem_mapping_add(registry, config, aligned, aligned_nbytes - 1, NULL);
	EXPECT_EQ("add(sub-host-page size)", err, 0);
	cudamem_mapping_clear(registry);

	/* Host-page aligned but sub-device-page aligned: accepted. */
	err = cudamem_mapping_add(registry, config, (uint8_t *)aligned + host_page,
				  aligned_nbytes - 2 * host_page, NULL);
	EXPECT_EQ("add(host-page aligned, sub-device-page)", err, 0);

	cudamem_mapping_clear(registry);
	cuMemFree(raw);
	return 0;
}

/* A 64-byte cuMemAlloc occupies only a fraction of its chunk, but the chunk
 * itself is fully VA-reserved. _add must export at chunk granularity, not at
 * the user's tight size, so registration of a sub-pagesize allocation
 * succeeds. */
static int
test_subpage_alloc(struct cudamem_mapping_registry *registry, struct cudamem_config *config)
{
	CUdeviceptr raw = 0;
	uint64_t phys = 0;
	int err;

	printf("# test_subpage_alloc\n");

	err = (cuMemAlloc(&raw, 64) == CUDA_SUCCESS) ? 0 : -ENOMEM;
	EXPECT_EQ("cuMemAlloc(64)", err, 0);

	err = cudamem_mapping_add(registry, config, (void *)raw, 64, NULL);
	EXPECT_EQ("add(64-byte buffer)", err, 0);

	err = cudamem_mapping_virt_to_phys(registry, (void *)raw, &phys);
	EXPECT_EQ("virt_to_phys(64-byte buffer)", err, 0);
	if (phys == 0) {
		printf("# FAILED: phys is zero\n");
		return 1;
	}

	cudamem_mapping_clear(registry);
	cuMemFree(raw);
	return 0;
}

static int
test_lookup_unmapped(struct cudamem_mapping_registry *registry, struct cudamem_config *config)
{
	const size_t page = (size_t)config->device_pagesize;
	const size_t gran = config->alloc_granularity;
	const size_t nbytes = 2 * page;
	CUdeviceptr raw = 0;
	void *aligned = NULL;
	size_t aligned_nbytes = 0;
	uint64_t phys = 0;
	int err;

	printf("# test_lookup_unmapped\n");

	err = (cuMemAlloc(&raw, nbytes) == CUDA_SUCCESS) ? 0 : -ENOMEM;
	EXPECT_EQ("cuMemAlloc", err, 0);
	aligned = (void *)raw;
	aligned_nbytes = nbytes;

	err = cudamem_mapping_add(registry, config, aligned, aligned_nbytes, NULL);
	EXPECT_EQ("cudamem_mapping_add", err, 0);

	/* One chunk below the registered range; expected unmapped. */
	err = cudamem_mapping_virt_to_phys(registry, (void *)((uint64_t)aligned - gran), &phys);
	EXPECT_EQ("virt_to_phys(chunk below registered)", err, -EINVAL);

	/* One chunk above the registered range; expected unmapped. */
	err = cudamem_mapping_virt_to_phys(registry, (uint8_t *)aligned + gran, &phys);
	EXPECT_EQ("virt_to_phys(chunk above registered)", err, -EINVAL);

	cudamem_mapping_clear(registry);
	cuMemFree(raw);
	return 0;
}

static int
test_multiple_mappings(struct cudamem_mapping_registry *registry, struct cudamem_config *config)
{
	const size_t gran = config->alloc_granularity;
	CUdeviceptr raw[3] = {0};
	void *aligned[3] = {0};
	size_t aligned_nbytes[3] = {0};
	uint64_t phys = 0;
	int err;

	printf("# test_multiple_mappings\n");

	/* Allocate gran bytes per mapping so each lands in (typically) its own
	 * chunk. Smaller per-allocation sizes get packed into a single chunk,
	 * which would cause the middle remove to leave the chunk live and
	 * allow the supposedly-removed mapping to still resolve. */
	for (int i = 0; i < 3; i++) {
		err = (cuMemAlloc(&raw[i], gran) == CUDA_SUCCESS) ? 0 : -ENOMEM;
		EXPECT_EQ("cuMemAlloc", err, 0);
		aligned[i] = (void *)raw[i];
		aligned_nbytes[i] = gran;

		err = cudamem_mapping_add(registry, config, aligned[i], aligned_nbytes[i], NULL);
		EXPECT_EQ("cudamem_mapping_add", err, 0);
	}

	for (int i = 0; i < 3; i++) {
		err = cudamem_mapping_virt_to_phys(registry, aligned[i], &phys);
		EXPECT_EQ("virt_to_phys", err, 0);
		if (phys == 0) {
			printf("# FAILED: phys is zero for mapping %d\n", i);
			return 1;
		}
	}

	/* Remove the middle one and verify the other two still resolve. */
	err = cudamem_mapping_remove(registry, aligned[1]);
	EXPECT_EQ("cudamem_mapping_remove(middle)", err, 0);

	err = cudamem_mapping_virt_to_phys(registry, aligned[1], &phys);
	EXPECT_EQ("virt_to_phys(after remove middle)", err, -EINVAL);

	err = cudamem_mapping_virt_to_phys(registry, aligned[0], &phys);
	EXPECT_EQ("virt_to_phys(0 still mapped)", err, 0);

	err = cudamem_mapping_virt_to_phys(registry, aligned[2], &phys);
	EXPECT_EQ("virt_to_phys(2 still mapped)", err, 0);

	cudamem_mapping_clear(registry);
	for (int i = 0; i < 3; i++) {
		cuMemFree(raw[i]);
	}
	return 0;
}

static int
test_descending_order_registration(struct cudamem_mapping_registry *registry,
				   struct cudamem_config *config)
{
	/* Register three distinct chunks in descending vaddr order. The LUT is
	 * indexed by absolute chunk_idx, so registration order should not
	 * affect resolution. Use gran-sized allocations to ensure each lands
	 * in a distinct chunk. */
	const size_t gran = config->alloc_granularity;
	CUdeviceptr raw[3] = {0};
	void *aligned[3] = {0};
	size_t aligned_nbytes[3] = {0};
	uint64_t phys = 0;
	int err;

	printf("# test_descending_order_registration\n");

	for (int i = 0; i < 3; i++) {
		err = (cuMemAlloc(&raw[i], gran) == CUDA_SUCCESS) ? 0 : -ENOMEM;
		EXPECT_EQ("cuMemAlloc", err, 0);
		aligned[i] = (void *)raw[i];
		aligned_nbytes[i] = gran;
	}

	/* Sort indices by vaddr descending so we register the highest first. */
	int order[3] = {0, 1, 2};
	for (int i = 0; i < 3; i++) {
		for (int j = i + 1; j < 3; j++) {
			if ((uint64_t)aligned[order[i]] < (uint64_t)aligned[order[j]]) {
				int tmp = order[i];
				order[i] = order[j];
				order[j] = tmp;
			}
		}
	}

	for (int i = 0; i < 3; i++) {
		err = cudamem_mapping_add(registry, config, aligned[order[i]],
					  aligned_nbytes[order[i]], NULL);
		EXPECT_EQ("cudamem_mapping_add", err, 0);
	}

	for (int i = 0; i < 3; i++) {
		err = cudamem_mapping_virt_to_phys(registry, aligned[i], &phys);
		EXPECT_EQ("virt_to_phys", err, 0);
		if (phys == 0) {
			printf("# FAILED: phys is zero for mapping %d\n", i);
			return 1;
		}
	}

	cudamem_mapping_clear(registry);
	for (int i = 0; i < 3; i++) {
		cuMemFree(raw[i]);
	}
	return 0;
}

static int
test_add_host_pointer(struct cudamem_mapping_registry *registry, struct cudamem_config *config)
{
	const size_t page = (size_t)config->device_pagesize;
	void *host = NULL;
	int err;

	printf("# test_add_host_pointer\n");

	/* Page-aligned host pointer passes _add's alignment check, but isn't GPU
	 * memory, so cuMemGetHandleForAddressRange must fail. Exercises the
	 * err_free unwind path inside _add. */
	host = aligned_alloc(page, 4 * page);
	if (!host) {
		printf("# FAILED: aligned_alloc; errno(%d)\n", errno);
		return 1;
	}

	err = cudamem_mapping_add(registry, config, host, 4 * page, NULL);
	if (err == 0) {
		printf("# FAILED: _add(host pointer) unexpectedly succeeded\n");
		cudamem_mapping_clear(registry);
		free(host);
		return 1;
	}
	printf("# _add(host pointer) rejected with err=%d\n", err);

	/* Registry must remain consistent (empty) after the failed _add. */
	if (registry->list != NULL) {
		printf("# FAILED: registry->list non-NULL after failed _add\n");
		free(host);
		return 1;
	}

	free(host);
	return 0;
}

/* Two registrations covering the same chunk: rc must track them, phys must
 * match, and the chunk is only released when both are removed. Drive this
 * via two cuMemAlloc calls of one host page each. The driver tends to pack
 * adjacent small allocations into the same chunk, but we don't depend on
 * that, the test self-detects whether the two allocations actually share a
 * chunk and asserts the right invariants either way. */
static int
test_chunk_sharing(struct cudamem_mapping_registry *registry, struct cudamem_config *config)
{
	const size_t page = (size_t)config->pagesize;
	const uint64_t mask = config->alloc_granularity - 1;
	CUdeviceptr raw_a = 0, raw_b = 0;
	void *a = NULL, *b = NULL;
	size_t a_nbytes = 0, b_nbytes = 0;
	uint64_t phys_before_a = 0, phys_before_b = 0;
	uint64_t phys_after_a = 0;
	int err;

	printf("# test_chunk_sharing\n");

	err = (cuMemAlloc(&raw_a, page) == CUDA_SUCCESS) ? 0 : -ENOMEM;
	EXPECT_EQ("cuMemAlloc(a)", err, 0);
	a = (void *)raw_a;
	a_nbytes = page;
	err = (cuMemAlloc(&raw_b, page) == CUDA_SUCCESS) ? 0 : -ENOMEM;
	EXPECT_EQ("cuMemAlloc(b)", err, 0);
	b = (void *)raw_b;
	b_nbytes = page;

	err = cudamem_mapping_add(registry, config, a, a_nbytes, NULL);
	EXPECT_EQ("add(a)", err, 0);
	err = cudamem_mapping_add(registry, config, b, b_nbytes, NULL);
	EXPECT_EQ("add(b)", err, 0);

	err = cudamem_mapping_virt_to_phys(registry, a, &phys_before_a);
	EXPECT_EQ("virt_to_phys(a)", err, 0);
	err = cudamem_mapping_virt_to_phys(registry, b, &phys_before_b);
	EXPECT_EQ("virt_to_phys(b)", err, 0);

	const int shared_chunk = (((uint64_t)a & ~mask) == ((uint64_t)b & ~mask));
	printf("# test_chunk_sharing: a=0x%" PRIxPTR " b=0x%" PRIxPTR " shared=%s\n", (uintptr_t)a,
	       (uintptr_t)b, shared_chunk ? "yes" : "no");

	err = cudamem_mapping_remove(registry, b);
	EXPECT_EQ("remove(b)", err, 0);

	/* a still resolves with the same phys whether or not b shared its
	 * chunk. */
	err = cudamem_mapping_virt_to_phys(registry, a, &phys_after_a);
	EXPECT_EQ("virt_to_phys(a after remove(b))", err, 0);
	if (phys_after_a != phys_before_a) {
		printf("# FAILED: phys for a changed across remove(b)\n");
		return 1;
	}

	/* When chunks were shared, b's vaddr is still resolvable through a's
	 * registration since the chunk is still cached. When not shared, b's
	 * chunk is now released and resolution must fail. */
	uint64_t phys_b_after_remove = 0;
	int b_after_err = cudamem_mapping_virt_to_phys(registry, b, &phys_b_after_remove);
	if (shared_chunk) {
		EXPECT_EQ("virt_to_phys(b after remove(b), shared chunk)", b_after_err, 0);
		if (phys_b_after_remove != phys_before_b) {
			printf("# FAILED: phys for b inside still-cached chunk drifted\n");
			return 1;
		}
	} else {
		EXPECT_EQ("virt_to_phys(b after remove(b), distinct chunk)", b_after_err, -EINVAL);
	}

	cudamem_mapping_clear(registry);
	cuMemFree(raw_a);
	cuMemFree(raw_b);
	return 0;
}

/* Registration that crosses a chunk boundary must populate every chunk it
 * touches, and the per-page virt_to_phys must produce contiguous phys
 * across the boundary so PRP construction works for buffers larger than a
 * single chunk. */
static int
test_cross_chunk_mapping(struct cudamem_mapping_registry *registry, struct cudamem_config *config)
{
	const size_t page = (size_t)config->pagesize;
	const size_t gran = config->alloc_granularity;
	CUdeviceptr raw = 0;
	void *aligned = NULL;
	size_t aligned_nbytes = 0;
	int err;

	printf("# test_cross_chunk_mapping\n");

	/* Allocate >gran bytes to span at least two chunks regardless of where
	 * cuMemAlloc places the start. */
	const size_t alloc_nbytes = gran + 16 * page;
	err = (cuMemAlloc(&raw, alloc_nbytes) == CUDA_SUCCESS) ? 0 : -ENOMEM;
	EXPECT_EQ("cuMemAlloc", err, 0);
	aligned = (void *)raw;
	aligned_nbytes = alloc_nbytes;

	err = cudamem_mapping_add(registry, config, aligned, aligned_nbytes, NULL);
	EXPECT_EQ("cudamem_mapping_add", err, 0);

	const size_t npages = aligned_nbytes / page;
	uint64_t prev_phys = 0;
	for (size_t i = 0; i < npages; ++i) {
		uint64_t phys = 0;
		err = cudamem_mapping_virt_to_phys(registry, (uint8_t *)aligned + i * page, &phys);
		EXPECT_EQ("virt_to_phys per-page", err, 0);
		if (i > 0 && phys != prev_phys + page) {
			/* Adjacent pages within a chunk are contiguous in phys
			 * (probe 12). The boundary between chunks is not
			 * required to be contiguous, since each chunk is its
			 * own BAR1 IOVA window. */
			const uintptr_t va = (uintptr_t)aligned + i * page;
			const uintptr_t prev_va = va - page;
			const int crossing_chunk_boundary = (va & ~((uintptr_t)gran - 1)) !=
							    (prev_va & ~((uintptr_t)gran - 1));
			if (!crossing_chunk_boundary) {
				printf("# FAILED: phys discontinuity within chunk at page %zu\n",
				       i);
				return 1;
			}
		}
		prev_phys = phys;
	}

	cudamem_mapping_clear(registry);
	cuMemFree(raw);
	return 0;
}

int
main(void)
{
	struct cudamem_config config = {0};
	struct cudamem_mapping_registry registry = {0};
	CUdevice cu_dev;
	CUcontext cu_ctx;
	int err;

	err = cuInit(0);
	if (err) {
		printf("# FAILED: cuInit(); err(%d)\n", err);
		return err;
	}

	err = cuDeviceGet(&cu_dev, 0);
	if (err) {
		printf("# FAILED: cuDeviceGet(); err(%d)\n", err);
		return err;
	}

	err = cuCtxCreate(&cu_ctx, 0, cu_dev);
	if (err) {
		printf("# FAILED: cuCtxCreate(); err(%d)\n", err);
		return err;
	}

	err = cudamem_config_init(&config, 0);
	if (err) {
		printf("# FAILED: cudamem_config_init(); err(%d)\n", err);
		goto exit_ctx;
	}

	err = cudamem_mapping_registry_init(&registry, &config);
	if (err) {
		printf("# FAILED: cudamem_mapping_registry_init(); err(%d)\n", err);
		goto exit_ctx;
	}

	err = test_add_remove_clear(&registry, &config);
	if (err) {
		goto exit;
	}
	err = test_unaligned_lookup(&registry, &config);
	if (err) {
		goto exit;
	}
	err = test_arbitrary_alignment(&registry, &config);
	if (err) {
		goto exit;
	}
	err = test_subpage_alloc(&registry, &config);
	if (err) {
		goto exit;
	}
	err = test_lookup_unmapped(&registry, &config);
	if (err) {
		goto exit;
	}
	err = test_multiple_mappings(&registry, &config);
	if (err) {
		goto exit;
	}
	err = test_descending_order_registration(&registry, &config);
	if (err) {
		goto exit;
	}
	err = test_add_host_pointer(&registry, &config);
	if (err) {
		goto exit;
	}
	err = test_chunk_sharing(&registry, &config);
	if (err) {
		goto exit;
	}
	err = test_cross_chunk_mapping(&registry, &config);
	if (err) {
		goto exit;
	}

	printf("SUCCES: all cudamem_mapping tests passed\n");

exit:
	cudamem_mapping_registry_term(&registry);
exit_ctx:
	cuCtxDestroy(cu_ctx);
	return err;
}
