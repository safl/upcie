// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * Exercise the registry's bookkeeping
 * ===================================
 *
 * Pure bookkeeping: adoption takes the addresses from a caller-supplied table,
 * so nothing here allocates device memory, exports a dma-buf, or touches a
 * device. What is checked is what the table promises, that a registered range
 * resolves and an unregistered one does not, and that the promise survives
 * removal and failure.
 */
#define _UPCIE_WITH_NVME
#include <upcie/upcie.h>

#define GRAN (2UL * 1024 * 1024)
#define GRAN_SHIFT 21
#define BASE 0x40000000UL ///< Granule aligned, well inside the default span

/**
 * A caller's finer-grained table, contiguous unless a break is asked for.
 *
 * `break_at` names a fine index whose address jumps, which is what adoption
 * must refuse: base + offset would otherwise resolve inside that granule to
 * an address the caller never gave us.
 */
static void
fill(uint64_t *lut, size_t n, size_t step, uint64_t phys, size_t break_at)
{
	for (size_t i = 0; i < n; ++i) {
		lut[i] = phys + (uint64_t)i * step;
		if (i >= break_at) {
			lut[i] += 0x100000000ULL;
		}
	}
}

static uint64_t
reg_addr(size_t granule)
{
	return 0xDDDD000000ULL + (uint64_t)granule * GRAN;
}

static int
test_adopt_resolves_and_remove_clears(void)
{
	struct dmamem_registry reg;
	uint64_t lut[GRAN / 4096 * 2];
	void *va = (void *)BASE;

	assert(!dmamem_registry_init(&reg, GRAN, 0, NULL, NULL, NULL, NULL));
	fill(lut, sizeof(lut) / sizeof(lut[0]), 4096, 0xAAAA000000ULL, (size_t)-1);

	if (dmamem_registry_adopt(&reg, va, GRAN * 2, lut, 12, NULL)) {
		printf("FAILED: adopting a contiguous range was refused\n");
		return 1;
	}

	if (reg.lut_phys[BASE >> GRAN_SHIFT] != 0xAAAA000000ULL) {
		printf("FAILED: the first granule did not take the caller's address\n");
		return 1;
	}
	if (!dmamem_registry_contains(&reg, va, GRAN * 2)) {
		printf("FAILED: the adopted range is not reported as contained\n");
		return 1;
	}

	if (dmamem_registry_remove(&reg, va)) {
		printf("FAILED: removing the registration was refused\n");
		return 1;
	}
	if (reg.lut_phys[BASE >> GRAN_SHIFT]) {
		printf("FAILED: removal left the granule resolving to an address\n");
		return 1;
	}

	dmamem_registry_term(&reg);
	printf("PASSED: adoption resolves, removal clears\n");
	return 0;
}

/**
 * A refused adoption must leave nothing behind.
 *
 * Adoption writes the table as it walks and can stop part-way, and a slot left
 * holding an address that no registration owns resolves to a plausible wrong
 * target rather than to the zero a caller reads as an error.
 */
static int
test_failed_adoption_leaves_no_addresses(void)
{
	struct dmamem_registry reg;
	uint64_t lut[GRAN / 4096 * 3];
	const size_t nfine = sizeof(lut) / sizeof(lut[0]);
	void *va = (void *)BASE;

	assert(!dmamem_registry_init(&reg, GRAN, 0, NULL, NULL, NULL, NULL));

	/* Break inside the third granule, so the first two are written before
	 * the walk refuses. */
	fill(lut, nfine, 4096, 0xBBBB000000ULL, (GRAN / 4096) * 2 + 4);

	if (!dmamem_registry_adopt(&reg, va, GRAN * 3, lut, 12, NULL)) {
		printf("FAILED: a discontiguous granule was accepted\n");
		return 1;
	}

	for (size_t k = 0; k < 3; ++k) {
		if (reg.lut_phys[(BASE >> GRAN_SHIFT) + k]) {
			printf("FAILED: granule %zu still resolves after a refused adoption\n", k);
			return 1;
		}
	}

	if (dmamem_registry_contains(&reg, va, GRAN)) {
		printf("FAILED: a refused adoption was recorded as contained\n");
		return 1;
	}

	dmamem_registry_term(&reg);
	printf("PASSED: a refused adoption leaves no addresses\n");
	return 0;
}

/**
 * Two backings must never share a granule.
 *
 * The table is indexed per granule, so an overlapping pair would each claim the
 * shared one, the second overwriting the first's address, and releasing either
 * would clear a granule the other still owns.
 */
static int
test_partial_overlap_is_refused(void)
{
	struct dmamem_registry reg;
	uint64_t lut[GRAN / 4096 * 3];
	const uint64_t first = reg_addr(0);

	assert(!dmamem_registry_init(&reg, GRAN, 0, NULL, NULL, NULL, NULL));
	fill(lut, sizeof(lut) / sizeof(lut[0]), 4096, 0xDDDD000000ULL, (size_t)-1);

	assert(!dmamem_registry_adopt(&reg, (void *)BASE, GRAN * 2, lut, 12, NULL));

	if (!dmamem_registry_adopt(&reg, (void *)(BASE + GRAN), GRAN * 2, lut, 12, NULL)) {
		printf("FAILED: a partially overlapping allocation was accepted\n");
		return 1;
	}

	if (reg.lut_phys[(BASE >> GRAN_SHIFT) + 1] != first + GRAN) {
		printf("FAILED: the refusal disturbed the granule it shares\n");
		return 1;
	}

	dmamem_registry_term(&reg);
	printf("PASSED: a partial overlap is refused\n");
	return 0;
}

static int
test_unaligned_base_and_capacity_are_refused(void)
{
	struct dmamem_registry reg;
	uint64_t lut[GRAN / 4096];

	assert(!dmamem_registry_init(&reg, GRAN, 0, NULL, NULL, NULL, NULL));
	fill(lut, sizeof(lut) / sizeof(lut[0]), 4096, 0xCCCC000000ULL, (size_t)-1);

	if (!dmamem_registry_adopt(&reg, (void *)(BASE + 4096), GRAN, lut, 12, NULL)) {
		printf("FAILED: an unaligned allocation base was accepted\n");
		return 1;
	}

	/* Past the span the table was sized for. */
	if (!dmamem_registry_adopt(&reg, (void *)(1ULL << 62), GRAN, lut, 12, NULL)) {
		printf("FAILED: an allocation beyond the table span was accepted\n");
		return 1;
	}

	dmamem_registry_term(&reg);
	printf("PASSED: unaligned and out-of-span registrations are refused\n");
	return 0;
}

int
main(void)
{
	if (test_adopt_resolves_and_remove_clears()) {
		return 1;
	}
	if (test_failed_adoption_leaves_no_addresses()) {
		return 1;
	}
	if (test_partial_overlap_is_refused()) {
		return 1;
	}
	if (test_unaligned_base_and_capacity_are_refused()) {
		return 1;
	}

	return 0;
}
