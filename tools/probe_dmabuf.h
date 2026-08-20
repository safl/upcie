// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * dma-buf export probe: what does a vendor runtime actually give us?
 * ==================================================================
 *
 * Flavour-agnostic body of upcie_probe_dmabuf_{cuda,hip}. A flavour defines the
 * functions prototyped below, includes this header, and calls probe_run(). The
 * prototypes are what state the contract: a flavour missing one, or defining it
 * with another signature, is diagnosed here rather than at the point of use.
 *
 * The questions this answers, per flavour, are in tools/README.md.
 *
 * @file probe_dmabuf.h
 */

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/**
 * Supplied by the flavour: export `nbytes` from `va` as a dma-buf descriptor
 *
 * @return 0 on success, the runtime's own error code otherwise.
 */
static int
probe_export(int *fd, uint64_t va, size_t nbytes);

/**
 * Supplied by the flavour: allocate `nbytes` of device memory
 *
 * @return 0 on success, the runtime's own error code otherwise.
 */
static int
probe_alloc(uint64_t *va, size_t nbytes);

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
probe_runs(struct dmabuf *dmabuf, size_t *nruns_out, uint64_t *granule_out)
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
probe_one(const char *what, uint64_t va, size_t nbytes)
{
	struct dmabuf attach = {0};
	uint64_t total = 0, shortest = ~0ULL, granule = 0;
	size_t nruns = 0;
	int fd = -1;
	int err;

	printf("  %-26s va(0x%" PRIx64 ") nbytes(%zu)\n", what, va, nbytes);

	err = probe_export(&fd, va, nbytes);
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
	probe_runs(&attach, &nruns, &granule);
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
probe_report_addrrange(const char *what, uint64_t va, uint64_t expect_base, size_t expect_size)
{
	uint64_t base = 0;
	size_t size = 0;
	int err;

	err = probe_addrrange(&base, &size, va);
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
probe_run(size_t size)
{
	uint64_t a = 0, b = 0;
	int err;

	err = probe_alloc(&a, size);
	if (err) {
		printf("probe_alloc() FAILED, rc(%d)\n", err);
		return err;
	}
	err = probe_alloc(&b, size);
	if (err) {
		printf("probe_alloc() FAILED, rc(%d)\n", err);
		return err;
	}

	printf("\nallocation A base(0x%" PRIx64 ") size(%zu)\n", a, size);
	probe_one("4K at base", a, 4096);
	probe_one("2M at base", a, 2UL << 20);
	probe_one("4K at base + 4K", a + 4096, 4096);
	probe_one("2M at base + 2M", a + (2UL << 20), 2UL << 20);
	probe_one("3M at base", a, 3UL << 20);
	probe_one("whole allocation", a, size);

	printf("\n  recovering the allocation from a pointer into it\n");
	probe_report_addrrange("at base", a, a, size);
	probe_report_addrrange("at base + 3M + 4K", a + (3UL << 20) + 4096, a, size);
	probe_report_addrrange("at last byte", a + size - 1, a, size);

	printf("\nallocation B base(0x%" PRIx64 ") size(%zu)\n", b, size);
	probe_one("4K at base", b, 4096);
	probe_one("whole allocation", b, size);

	return 0;
}
