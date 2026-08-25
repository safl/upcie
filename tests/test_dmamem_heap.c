// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * Exercise the granule guarantee of dmamem_heap
 * =============================================
 *
 * Pure allocator logic: the dmamem is a descriptor with a size and a
 * translator, and only offsets are checked, so no device is needed.
 */
#define _UPCIE_WITH_NVME
#include <upcie/upcie.h>

#define HUGEPGSZ (2UL * 1024 * 1024)
#define QUEUE_BYTES (1024UL * 64)
#define PRP_BYTES (1024UL * 4096)

static void
heap_lut_init(struct dmamem *dmem, struct dmamem_heap *heap)
{
	memset(dmem, 0, sizeof(*dmem));
	dmem->size = 512UL * 1024 * 1024;
	dmem->translator = DMAMEM_XLATE_LUT;
	dmem->registry.gran_shift = 21;
	dmem->registry.gran_mask = HUGEPGSZ - 1;

	assert(!dmamem_heap_init(heap, dmem, 4096));
}

static int
straddles(size_t offset, size_t nbytes)
{
	return ((offset & (HUGEPGSZ - 1)) + nbytes) > HUGEPGSZ;
}

/**
 * Replay the controller bring-up order, varying how many data buffers are
 * taken from the same heap first; the buffers are what shift the queues off
 * the grid that bring-up alone would keep them on.
 */
static int
test_queues_never_straddle(void)
{
	for (size_t nbufs = 0; nbufs < 512; ++nbufs) {
		struct dmamem_heap heap;
		struct dmamem dmem;
		size_t off;

		heap_lut_init(&dmem, &heap);

		assert(!dmamem_heap_alloc_array_aligned(&heap, 1, QUEUE_BYTES, 4096, &off));
		assert(!dmamem_heap_alloc_array_aligned(&heap, 1, QUEUE_BYTES, 4096, &off));
		assert(!dmamem_heap_alloc_array_aligned(&heap, NVME_REQUEST_POOL_LEN, 4096, 4096,
						       &off));

		for (size_t i = 0; i < nbufs; ++i) {
			assert(!dmamem_heap_alloc_aligned(&heap, 4096, 4096, &off));
		}

		assert(!dmamem_heap_alloc_array_aligned(&heap, 1, QUEUE_BYTES, 4096, &off));
		if (straddles(off, QUEUE_BYTES)) {
			printf("FAILED: sq at 0x%zx straddles after %zu buffers\n", off, nbufs);
			return 1;
		}

		assert(!dmamem_heap_alloc_array_aligned(&heap, 1, QUEUE_BYTES, 4096, &off));
		if (straddles(off, QUEUE_BYTES)) {
			printf("FAILED: cq at 0x%zx straddles after %zu buffers\n", off, nbufs);
			return 1;
		}

		dmamem_heap_term(&heap);
	}

	printf("PASSED: queues stay within a granule across 512 layouts\n");
	return 0;
}

/**
 * Negative control: the unconstrained call may cross a boundary, and over
 * the same layouts it does, so the guarantee above is not vacuous.
 */
static int
test_plain_alloc_may_straddle(void)
{
	size_t crossings = 0;

	for (size_t nbufs = 0; nbufs < 512; ++nbufs) {
		struct dmamem_heap heap;
		struct dmamem dmem;
		size_t off;

		heap_lut_init(&dmem, &heap);

		assert(!dmamem_heap_alloc_aligned(&heap, QUEUE_BYTES, 4096, &off));
		assert(!dmamem_heap_alloc_aligned(&heap, QUEUE_BYTES, 4096, &off));
		assert(!dmamem_heap_alloc_aligned(&heap, PRP_BYTES, 4096, &off));

		for (size_t i = 0; i < nbufs; ++i) {
			assert(!dmamem_heap_alloc_aligned(&heap, 4096, 4096, &off));
		}

		assert(!dmamem_heap_alloc_aligned(&heap, QUEUE_BYTES, 4096, &off));
		crossings += straddles(off, QUEUE_BYTES);

		dmamem_heap_term(&heap);
	}

	if (!crossings) {
		printf("FAILED: no layout crossed a boundary; the guarantee is untested\n");
		return 1;
	}

	printf("PASSED: unconstrained allocation crosses boundaries (%zu of 512 layouts)\n",
	       crossings);
	return 0;
}

/**
 * An array of pages may span granules; each page may not.
 */
static int
test_array_elements_never_straddle(void)
{
	struct dmamem_heap heap;
	struct dmamem dmem;
	size_t off;

	heap_lut_init(&dmem, &heap);

	assert(!dmamem_heap_alloc_aligned(&heap, 4096 * 3, 4096, &off));
	assert(!dmamem_heap_alloc_array_aligned(&heap, NVME_REQUEST_POOL_LEN, 4096, 4096, &off));

	if (PRP_BYTES <= HUGEPGSZ) {
		printf("FAILED: scratch no longer spans a granule; test says nothing\n");
		return 1;
	}

	for (size_t i = 0; i < NVME_REQUEST_POOL_LEN; ++i) {
		const size_t page = off + (i * 4096);

		if (straddles(page, 4096)) {
			printf("FAILED: scratch page %zu at 0x%zx straddles\n", i, page);
			return 1;
		}
	}

	dmamem_heap_term(&heap);

	printf("PASSED: array spans granules, its elements do not\n");
	return 0;
}

/**
 * Asking for contiguity beyond a granule fails rather than tearing.
 */
static int
test_impossible_requests_are_refused(void)
{
	struct dmamem_heap heap;
	struct dmamem dmem;
	size_t off;

	heap_lut_init(&dmem, &heap);

	if (-EINVAL != dmamem_heap_alloc_array_aligned(&heap, 1, HUGEPGSZ * 2, 4096, &off)) {
		printf("FAILED: an element larger than a granule was accepted\n");
		return 1;
	}

	/* 3 pages does not divide a granule: no untearing layout exists. */
	if (-EINVAL != dmamem_heap_alloc_array_aligned(&heap, 4096, 4096 * 3, 4096, &off)) {
		printf("FAILED: a tearing array layout was accepted\n");
		return 1;
	}

	dmamem_heap_term(&heap);

	printf("PASSED: impossible promises are refused\n");
	return 0;
}

/**
 * Under ARITHMETIC translation there is no granule to respect.
 */
static int
test_arithmetic_is_unconstrained(void)
{
	struct dmamem_heap heap;
	struct dmamem dmem;
	size_t off;

	memset(&dmem, 0, sizeof(dmem));
	dmem.size = 512UL * 1024 * 1024;
	dmem.translator = DMAMEM_XLATE_ARITHMETIC;

	assert(!dmamem_heap_init(&heap, &dmem, 4096));

	if (dmamem_heap_alloc_array_aligned(&heap, 1, HUGEPGSZ * 4, 4096, &off)) {
		printf("FAILED: a contiguous IOVA range refused a large element\n");
		return 1;
	}

	dmamem_heap_term(&heap);

	printf("PASSED: arithmetic translation is unconstrained\n");
	return 0;
}

int
main(void)
{
	if (test_queues_never_straddle()) {
		return 1;
	}
	if (test_plain_alloc_may_straddle()) {
		return 1;
	}
	if (test_array_elements_never_straddle()) {
		return 1;
	}
	if (test_impossible_requests_are_refused()) {
		return 1;
	}
	if (test_arithmetic_is_unconstrained()) {
		return 1;
	}

	return 0;
}
