// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * Exercise the qid bitmap accounting
 * ==================================
 *
 * Pure bitmap logic: the allocator is a bitmap and a handful of index
 * calculations, so no device is needed.
 */
#define _UPCIE_WITH_NVME
#include <upcie/upcie.h>

static int
test_init_reserves_the_admin_queue(void)
{
	uint64_t bitmap[NVME_QID_BITMAP_WORDS];

	assert(!nvme_qid_bitmap_init(bitmap));

	if (nvme_qid_used(bitmap) != 1) {
		printf("FAILED: a freshly initialised bitmap should hold qid 0 alone\n");
		return 1;
	}

	printf("PASSED: init reserves the admin queue\n");
	return 0;
}

static int
test_used_tracks_alloc_and_free(void)
{
	uint64_t bitmap[NVME_QID_BITMAP_WORDS];

	assert(!nvme_qid_bitmap_init(bitmap));

	for (uint16_t qid = 1; qid <= 8; ++qid) {
		assert(!nvme_qid_alloc(bitmap, qid));

		/* The admin queue is counted too, so the total runs one ahead */
		if (nvme_qid_used(bitmap) != (uint32_t)qid + 1) {
			printf("FAILED: after allocating %u ids, used is %u\n", qid,
			       nvme_qid_used(bitmap));
			return 1;
		}
	}

	for (uint16_t qid = 8; qid >= 1; --qid) {
		assert(!nvme_qid_free(bitmap, qid));

		if (nvme_qid_used(bitmap) != (uint32_t)qid) {
			printf("FAILED: after freeing down to %u, used is %u\n", qid,
			       nvme_qid_used(bitmap));
			return 1;
		}
	}

	printf("PASSED: used tracks alloc and free\n");
	return 0;
}

static int
test_used_counts_across_words(void)
{
	uint64_t bitmap[NVME_QID_BITMAP_WORDS];
	const uint16_t qids[] = {1, 63, 64, 65, 127, 128, 1023, 1024};
	uint32_t expect = 1; ///< The admin queue

	assert(!nvme_qid_bitmap_init(bitmap));

	/* Straddling word boundaries is where an index calculation goes wrong
	 * without the count noticing, so place ids either side of several. */
	for (size_t i = 0; i < sizeof(qids) / sizeof(qids[0]); ++i) {
		assert(!nvme_qid_alloc(bitmap, qids[i]));
		expect++;

		if (nvme_qid_used(bitmap) != expect) {
			printf("FAILED: qid %u brought used to %u, expected %u\n", qids[i],
			       nvme_qid_used(bitmap), expect);
			return 1;
		}
	}

	printf("PASSED: used counts across word boundaries\n");
	return 0;
}

static int
test_every_admitted_qid_is_representable(void)
{
	uint64_t bitmap[NVME_QID_BITMAP_WORDS + 1];

	/* The bounds checks admit qid < NVME_QID_MAX, so the bitmap has to hold
	 * every one of them. Truncating the word count instead of rounding up
	 * leaves the top of that range indexing past the end.
	 *
	 * The guard word is zero rather than a pattern: allocating only ever
	 * sets a bit, so any write lands in a word that should have stayed
	 * clear, whereas a pattern can already carry the bit being set. */
	assert(!nvme_qid_bitmap_init(bitmap));
	bitmap[NVME_QID_BITMAP_WORDS] = 0;

	if (nvme_qid_alloc(bitmap, NVME_QID_MAX - 1)) {
		printf("FAILED: the highest admitted qid was refused\n");
		return 1;
	}

	if (bitmap[NVME_QID_BITMAP_WORDS]) {
		printf("FAILED: allocating qid %u wrote past the end of the bitmap\n",
		       NVME_QID_MAX - 1);
		return 1;
	}

	if (nvme_qid_used(bitmap) != 2) {
		printf("FAILED: the highest admitted qid was not counted\n");
		return 1;
	}

	printf("PASSED: every admitted qid is representable\n");
	return 0;
}

static int
test_allocating_twice_counts_once(void)
{
	uint64_t bitmap[NVME_QID_BITMAP_WORDS];

	assert(!nvme_qid_bitmap_init(bitmap));
	assert(!nvme_qid_alloc(bitmap, 7));
	assert(!nvme_qid_alloc(bitmap, 7));

	if (nvme_qid_used(bitmap) != 2) {
		printf("FAILED: allocating the same id twice counted %u\n", nvme_qid_used(bitmap));
		return 1;
	}

	printf("PASSED: allocating the same id twice counts once\n");
	return 0;
}

int
main(void)
{
	if (test_init_reserves_the_admin_queue()) {
		return 1;
	}
	if (test_used_tracks_alloc_and_free()) {
		return 1;
	}
	if (test_used_counts_across_words()) {
		return 1;
	}
	if (test_every_admitted_qid_is_representable()) {
		return 1;
	}
	if (test_allocating_twice_counts_once()) {
		return 1;
	}

	return 0;
}
