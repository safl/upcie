// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * Translating through memory somebody else described
 * ==================================================
 *
 * A consumer of a delegated runtime maps the owner's memory and has to turn
 * addresses in it into DMA addresses, without reading pagemap. It does that
 * from a description the owner left behind.
 *
 * The interesting part is not the copy but the rebasing: a registry is indexed
 * by address, and the consumer's mapping is at a different address than the
 * owner's. So this maps the same descriptor twice, builds a second dmamem over
 * the second mapping from the description alone, and requires that both agree
 * on the DMA address of the same byte. No second process is needed to show
 * that, and none is used, so what fails here is the translation rather than
 * anything about sockets.
 *
 * Usage:
 *   test_dmamem_shared_hostmem
 */
#include <upcie/upcie.h>

#define NBYTES (4 * 1024 * 1024)

int
main(void)
{
	struct hostmem_shared_desc *desc;
	struct hostmem_config config = {0};
	struct hostmem_hugepage hp = {0};
	struct dmamem owned = {0};
	struct dmamem shared = {0};
	size_t probes[] = {0, 4096, NBYTES / 2, NBYTES - 4096};
	void *second;
	int err;

	hostmem_config_init(&config);

	err = hostmem_hugepage_alloc(NBYTES, &hp, &config);
	if (err) {
		printf("# FAILED: hostmem_hugepage_alloc(); err(%d)\n", err);
		return 1;
	}

	err = dmamem_from_hostmem_registry(&owned, &hp, 0);
	if (err) {
		printf("# FAILED: dmamem_from_hostmem_registry(); err(%d)\n", err);
		return 1;
	}

	desc = malloc(hostmem_shared_desc_nbytes(hp.nphys));
	if (!desc || hostmem_shared_desc_fill(desc, &hp)) {
		printf("# FAILED: describe the region\n");
		return 1;
	}
	printf("described %u granules of 2^%u bytes, %llu total\n", desc->nphys, desc->gran_shift,
	       (unsigned long long)desc->nbytes);

	/* Another mapping of the same memory, standing in for another process's
	 * view of it: same pages, different addresses. */
	second = mmap(NULL, hp.size, PROT_READ | PROT_WRITE, MAP_SHARED, hp.fd, 0);
	if (second == MAP_FAILED) {
		printf("# FAILED: mmap(second); errno(%d)\n", errno);
		return 1;
	}
	if (second == hp.virt) {
		printf("# FAILED: the second mapping landed at the first's address\n");
		return 1;
	}

	err = dmamem_from_shared_hostmem(&shared, second, desc, 0);
	if (err) {
		printf("# FAILED: dmamem_from_shared_hostmem(); err(%d)\n", err);
		return 1;
	}

	for (size_t i = 0; i < sizeof(probes) / sizeof(*probes); ++i) {
		uint64_t a = dmamem_offset_to_iova(&owned, probes[i]);
		uint64_t b = dmamem_offset_to_iova(&shared, probes[i]);

		if (a != b) {
			printf("# FAILED: offset 0x%zx resolves to 0x%" PRIx64
			       " here and 0x%" PRIx64 " there\n",
			       probes[i], a, b);
			return 1;
		}
	}
	printf("# LGTM: both mappings agree on where every probed offset lives\n");

	/* And the memory really is the same memory. */
	memset((char *)hp.virt + 8192, 0xA5, 4096);
	if (memcmp((char *)hp.virt + 8192, (char *)second + 8192, 4096)) {
		printf("# FAILED: the two mappings are not the same pages\n");
		return 1;
	}
	printf("# LGTM: and it is the same memory\n");

	dmamem_destroy(&shared);
	munmap(second, hp.size);
	free(desc);
	dmamem_destroy(&owned);
	hostmem_hugepage_free(&hp);

	return 0;
}
