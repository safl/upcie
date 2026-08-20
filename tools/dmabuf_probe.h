// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * dma-buf export probe: what does a vendor runtime actually give us?
 * ==================================================================
 *
 * Flavour-agnostic body of upcie_dmabuf_probe_{cuda,hip}. The flavour supplies
 * dmabuf_probe_export() and dmabuf_probe_alloc(), then calls
 * dmabuf_probe_run().
 *
 * The questions this answers, per flavour, are in tools/README.md.
 *
 * @file dmabuf_probe.h
 */

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/**
 * Collapse the scatter list into maximal physically contiguous runs.
 *
 * An exporter may split a run at its own page size even when the addresses are
 * adjacent, so the segment count overstates fragmentation. What a translator
 * cares about is where the addresses actually jump.
 *
 * @param nruns_out  Number of maximal contiguous runs.
 * @param granule_out Largest power-of-two granule the list may be indexed by:
 *        the largest power of two dividing every run boundary, since a granule
 *        window is safe exactly when no jump falls inside it. When there is a
 *        single run this is the described length, meaning no jump anywhere.
 */
static void
dmabuf_probe_runs(struct dmabuf *dmabuf, size_t *nruns_out, uint64_t *granule_out)
{
	uint64_t off = 0, divisor = 0;
	size_t nruns = 1;

	for (size_t j = 0; j < dmabuf->npages; ++j) {
		if (j && (dmabuf->pages[j].addr !=
			  dmabuf->pages[j - 1].addr + dmabuf->pages[j - 1].len)) {
			divisor |= off;
			nruns++;
		}
		off += dmabuf->pages[j].len;
	}
	divisor |= off;

	*nruns_out = nruns;
	/* Lowest set bit: the largest power of two dividing every boundary. */
	*granule_out = divisor & (~divisor + 1);
}

/**
 * Export [va, va + nbytes) and report what came back.
 *
 * `expect_nbytes` is what a caller would assume the export describes.
 */
static void
dmabuf_probe_one(const char *what, uint64_t va, size_t nbytes)
{
	struct dmabuf attach = {0};
	uint64_t total = 0, shortest = ~0ULL, granule = 0;
	size_t nruns = 0;
	int fd = -1;
	int err;

	printf("  %-26s va(0x%" PRIx64 ") nbytes(%zu)\n", what, va, nbytes);

	err = dmabuf_probe_export(&fd, va, nbytes);
	if (err) {
		printf("      export FAILED, rc(%d)\n", err);
		return;
	}

	err = dmabuf_import_attach(fd, &attach);
	if (err) {
		printf("      dmabuf_import_attach() FAILED, err(%d)\n", err);
		close(fd);
		return;
	}

	for (size_t j = 0; j < attach.npages; ++j) {
		total += attach.pages[j].len;
		if (attach.pages[j].len < shortest) {
			shortest = attach.pages[j].len;
		}
	}

	printf("      nsegments(%zu) described(%" PRIu64 ") %s requested(%zu)\n", attach.npages,
	       total, total == nbytes ? "==" : "!=", nbytes);
	dmabuf_probe_runs(&attach, &nruns, &granule);
	printf("      first_addr(0x%" PRIx64 ") shortest_seg(%" PRIu64
	       ") phys_runs(%zu) granule(%" PRIu64 ")\n",
	       attach.pages[0].addr, shortest, nruns, granule);

	dmabuf_import_detach(&attach);
}

/**
 * Can the allocation a pointer falls in be recovered from the pointer?
 *
 * A per-allocation export is only usable if so: the exported scatter list is
 * indexed from the base of the allocation, and a caller registering a
 * sub-range has to be placed at the right offset within it.
 */
static void
dmabuf_probe_addressrange(const char *what, uint64_t va, uint64_t expect_base, size_t expect_size)
{
	uint64_t base = 0;
	size_t size = 0;
	int err;

	err = dmabuf_probe_addrrange(&base, &size, va);
	if (err) {
		printf("  %-26s va(0x%" PRIx64 ") -> FAILED rc(%d)\n", what, va, err);
		return;
	}

	printf("  %-26s va(0x%" PRIx64 ") -> base(0x%" PRIx64 ") size(%zu) base %s size %s\n",
	       what, va, base, size, base == expect_base ? "ok" : "WRONG",
	       size == expect_size ? "ok" : "WRONG");
}

/**
 * Probe two separate device allocations of `size` bytes each.
 */
static int
dmabuf_probe_run(size_t size)
{
	uint64_t a = 0, b = 0;
	int err;

	err = dmabuf_probe_alloc(&a, size);
	if (err) {
		printf("dmabuf_probe_alloc() FAILED, rc(%d)\n", err);
		return err;
	}
	err = dmabuf_probe_alloc(&b, size);
	if (err) {
		printf("dmabuf_probe_alloc() FAILED, rc(%d)\n", err);
		return err;
	}

	printf("\nallocation A base(0x%" PRIx64 ") size(%zu)\n", a, size);
	dmabuf_probe_one("4K at base", a, 4096);
	dmabuf_probe_one("2M at base", a, 2UL << 20);
	dmabuf_probe_one("4K at base + 4K", a + 4096, 4096);
	dmabuf_probe_one("2M at base + 2M", a + (2UL << 20), 2UL << 20);
	dmabuf_probe_one("3M at base", a, 3UL << 20);
	dmabuf_probe_one("whole allocation", a, size);

	printf("\n  an odd-sized allocation\n");
	{
		const size_t odd = (3UL << 20) + 4096;
		uint64_t c = 0, base = 0;
		size_t rsize = 0;

		if (dmabuf_probe_alloc(&c, odd)) {
			printf("    allocation FAILED\n");
		} else {
			printf("    requested(%zu) base(0x%" PRIx64 ") base%%2M(%" PRIu64 ")\n",
			       odd, c, c % (2UL << 20));
			if (dmabuf_probe_addrrange(&base, &rsize, c)) {
				printf("    range recovery FAILED\n");
			} else {
				printf("    recovered size(%zu) rounded_up_to_2M(%s)\n", rsize,
				       rsize % (2UL << 20) ? "no" : "yes");
			}
			dmabuf_probe_one("whole odd allocation", c, rsize ? rsize : odd);
		}
	}

	printf("\n  recovering the allocation from a pointer into it\n");
	dmabuf_probe_addressrange("at base", a, a, size);
	dmabuf_probe_addressrange("at base + 3M + 4K", a + (3UL << 20) + 4096, a, size);
	dmabuf_probe_addressrange("at last byte", a + size - 1, a, size);

	printf("\nallocation B base(0x%" PRIx64 ") size(%zu)\n", b, size);
	dmabuf_probe_one("4K at base", b, 4096);
	dmabuf_probe_one("whole allocation", b, size);

	return 0;
}
