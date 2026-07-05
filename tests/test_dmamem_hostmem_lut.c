// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * dmamem_from_hostmem_lut smoketest
 * =================================
 *
 * Exercises the LUT translator over an existing hostmem_hugepage: no
 * IOMMU mapping is installed, so no iommufd handle is opened; the
 * dmamem borrows the hugepage's phys_lut and translates each submission
 * VA to a PA via table lookup + intra-hugepage offset.
 *
 * The test allocates a multi-hugepage hostmem region, wraps it via
 * dmamem_from_hostmem_lut, then probes several VAs across the region:
 *
 *   - the base of each backing hugepage,
 *   - an arbitrary interior offset within a hugepage,
 *   - the last byte of the last hugepage.
 *
 * For each probe, the LUT-translated IOVA is compared against the
 * pagemap-derived PA (phys_lut[i] + intra-page offset). The two must
 * match; if they diverge, the LUT translator or the hugepage_alloc
 * pagemap-population is broken.
 *
 * Requires CAP_SYS_ADMIN so hostmem_hugepage_alloc can read
 * /proc/self/pagemap and populate phys_lut. A hugepage pool with the
 * requested count reserved. No NVMe device, no iommufd, no vfio; just
 * hostmem + dmamem.
 *
 * On success the program prints "OK" and exits with 0.
 */
#include <upcie/upcie.h>

static int
probe(struct dmamem *dmem, struct hostmem_hugepage *hp, size_t offset)
{
	void *va = (uint8_t *)dmem->cpu_va + offset;
	size_t hp_idx = offset / hp->config->hugepgsz;
	size_t in_hp_offset = offset & (hp->config->hugepgsz - 1);
	uint64_t iova_via_lut = dmamem_va_to_iova(dmem, va);
	uint64_t iova_expected = hp->phys_lut[hp_idx] + in_hp_offset;

	if (iova_via_lut != iova_expected) {
		fprintf(stderr,
			"FAIL: offset=%zu hp_idx=%zu in_hp=%zu iova(0x%" PRIx64
			") != phys_lut[%zu](0x%" PRIx64 ") + in_hp(0x%zx)\n",
			offset, hp_idx, in_hp_offset, iova_via_lut, hp_idx, hp->phys_lut[hp_idx],
			in_hp_offset);
		return -EINVAL;
	}

	printf("probe: offset=%-10zu hp_idx=%zu iova=0x%016" PRIx64 " (phys_lut[%zu]=0x%" PRIx64
	       " + 0x%zx)\n",
	       offset, hp_idx, iova_via_lut, hp_idx, hp->phys_lut[hp_idx], in_hp_offset);
	return 0;
}

static int
smoketest(size_t hugepgsz, size_t nhugepages)
{
	struct hostmem_config config = {0};
	struct hostmem_hugepage hp = {0};
	struct dmamem dmem = {0};
	size_t region_size = hugepgsz * nhugepages;
	int err;

	err = hostmem_config_init(&config);
	if (err) {
		fprintf(stderr, "FAIL: hostmem_config_init() err(%d)\n", err);
		return err;
	}

	err = hostmem_hugepage_alloc(region_size, &hp, &config);
	if (err) {
		fprintf(stderr, "FAIL: hostmem_hugepage_alloc() err(%d)\n", err);
		return err;
	}
	if (!hp.phys_lut) {
		fprintf(stderr,
			"FAIL: phys_lut is NULL (pagemap read likely denied; need CAP_SYS_ADMIN)\n");
		hostmem_hugepage_free(&hp);
		return -EPERM;
	}

	err = dmamem_from_hostmem_lut(&dmem, &hp);
	if (err) {
		fprintf(stderr, "FAIL: dmamem_from_hostmem_lut() err(%d)\n", err);
		hostmem_hugepage_free(&hp);
		return err;
	}
	dmamem_pp(&dmem);

	if (dmem.translator != DMAMEM_XLATE_LUT) {
		fprintf(stderr, "FAIL: expected LUT translator, got %d\n", dmem.translator);
		err = -EINVAL;
		goto out;
	}
	if (dmem.owned) {
		fprintf(stderr, "FAIL: wrapping dmamem should have owned=0\n");
		err = -EINVAL;
		goto out;
	}
	if (dmem.cpu_va != hp.virt || dmem.size != hp.size) {
		fprintf(stderr, "FAIL: wrapping fields do not match hostmem_hugepage\n");
		err = -EINVAL;
		goto out;
	}
	if (dmem.phys_lut != hp.phys_lut) {
		fprintf(stderr, "FAIL: dmem.phys_lut is not borrowed from hp.phys_lut\n");
		err = -EINVAL;
		goto out;
	}

	/* Probe: base of each hugepage. */
	for (size_t i = 0; i < nhugepages; ++i) {
		err = probe(&dmem, &hp, i * hugepgsz);
		if (err) {
			goto out;
		}
	}

	/* Probe: an interior offset inside each hugepage. */
	for (size_t i = 0; i < nhugepages; ++i) {
		err = probe(&dmem, &hp, i * hugepgsz + 0x1234);
		if (err) {
			goto out;
		}
	}

	/* Probe: last byte of the region. */
	err = probe(&dmem, &hp, region_size - 1);
	if (err) {
		goto out;
	}

	printf("OK: LUT translator agrees with pagemap over %zu hugepage(s) of %zu bytes\n",
	       nhugepages, hugepgsz);

out:
	dmamem_destroy(&dmem);
	hostmem_hugepage_free(&hp);
	return err;
}

int
main(int argc, char *argv[])
{
	size_t hugepgsz = 2ULL * 1024 * 1024;
	size_t nhugepages = 4;

	(void)argc;
	(void)argv;

	return smoketest(hugepgsz, nhugepages) ? 1 : 0;
}
