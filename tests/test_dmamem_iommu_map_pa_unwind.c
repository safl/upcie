// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * dmamem_iommu_map_pa error paths, without the module
 * ===================================================
 *
 * The decorator reaches the iommu-map-pa module only through ioctl(), and
 * allocates with calloc() and free(), all inline in the headers. So this test
 * stands in for the three before including them: ioctl() hands out map
 * handles, fails the add it is told to, and counts a delete of a handle it no
 * longer holds; the allocator counts what is outstanding. The module's UAPI
 * header is taken from the source tree, so the real wrappers are compiled in
 * whether or not the DKMS package is installed.
 *
 * Checks that dmamem_iommu_map_pa_populate() gives back everything it took
 * when the inner backend fails, and that dmamem_iommu_map_pa_attach(), when a
 * back-fill fails, leaves no handle that a later
 * dmamem_iommu_map_pa_release() deletes a second time.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../experimental/iommu_map_pa/module/iommu_map_pa.h"

#define NHANDLES 64

static long g_nalloc;
static int g_nadd;
static int g_fail_add = -1;
static int g_live[NHANDLES];
static uint64_t g_next_handle;
static int g_stale_del;

static void *
counted_calloc(size_t nmemb, size_t size)
{
	void *ptr = calloc(nmemb, size);

	if (ptr) {
		g_nalloc++;
	}

	return ptr;
}

static void
counted_free(void *ptr)
{
	if (ptr) {
		g_nalloc--;
	}
	free(ptr);
}

static int
fake_ioctl(int fd, unsigned long request, ...)
{
	va_list ap;
	void *arg;

	(void)fd;
	va_start(ap, request);
	arg = va_arg(ap, void *);
	va_end(ap);

	if (request == IOMMU_MAP_PA) {
		struct iommu_map_pa_req *req = arg;

		if (g_nadd++ == g_fail_add || g_next_handle + 1 >= NHANDLES) {
			errno = EIO;
			return -1;
		}
		req->map_handle = ++g_next_handle;
		g_live[req->map_handle] = 1;

		return 0;
	}
	if (request == IOMMU_UNMAP_PA) {
		struct iommu_unmap_pa_req *req = arg;

		if (req->map_handle >= NHANDLES || !g_live[req->map_handle]) {
			g_stale_del++;
			errno = ENOENT;
			return -1;
		}
		g_live[req->map_handle] = 0;

		return 0;
	}

	errno = ENOTTY;
	return -1;
}

#define calloc counted_calloc
#define free counted_free
#define ioctl fake_ioctl

#define _UPCIE_WITH_NVME
#include <upcie/upcie.h>

#define GRAN 4096UL
#define NLUT 4

static int g_inner_err;
static int g_next_fd = 100;

static int
inner_populate(void *ctx, uint64_t base, size_t size, uint64_t granularity, uint64_t *lut_out,
	       size_t nlut, struct dmabuf *attach_out)
{
	(void)ctx;
	(void)size;

	if (g_inner_err) {
		return g_inner_err;
	}
	for (size_t i = 0; i < nlut; ++i) {
		lut_out[i] = base + i * granularity;
	}
	attach_out->fd = g_next_fd++;

	return 0;
}

static void
imp_init(struct dmamem_iommu_map_pa *imp)
{
	memset(imp, 0, sizeof(*imp));
	imp->fd = 3;
	strcpy(imp->bdfs[0], "0000:01:00.0");
	imp->nbdfs = 1;
	imp->window_base = 0x100000000ULL;
	imp->window_size = 1ULL << 30;
	imp->populate = inner_populate;
}

static int
live_handles(void)
{
	int n = 0;

	for (int i = 0; i < NHANDLES; ++i) {
		n += g_live[i];
	}

	return n;
}

static int
test_populate_unwinds(void)
{
	struct dmamem_iommu_map_pa imp;
	struct dmabuf attach = {.fd = -1};
	uint64_t lut[NLUT];
	int err;

	imp_init(&imp);
	g_inner_err = -EIO;

	err = dmamem_iommu_map_pa_populate(&imp, 0x200000, NLUT * GRAN, GRAN, lut, NLUT, &attach);
	g_inner_err = 0;
	if (err != -EIO) {
		fprintf(stderr, "FAIL: populate gave err(%d), expected -EIO\n", err);
		return 1;
	}
	if (g_nalloc) {
		fprintf(stderr, "FAIL: populate left %ld allocation(s) behind\n", g_nalloc);
		return 1;
	}

	printf("OK: a failed inner populate gives back everything\n");
	return 0;
}

static int
test_attach_unwinds(void)
{
	struct dmamem_iommu_map_pa imp;
	struct dmabuf attach[2];
	uint64_t lut[NLUT];
	int err;

	imp_init(&imp);

	for (int i = 0; i < 2; ++i) {
		memset(&attach[i], 0, sizeof(attach[i]));
		err = dmamem_iommu_map_pa_populate(&imp, 0x200000 * (i + 1), NLUT * GRAN, GRAN,
						   lut, NLUT, &attach[i]);
		if (err) {
			fprintf(stderr, "FAIL: populate(%d) err(%d)\n", i, err);
			return 1;
		}
	}

	g_fail_add = g_nadd + 1;
	err = dmamem_iommu_map_pa_attach(&imp, "0000:02:00.0");
	g_fail_add = -1;
	if (err != -EIO) {
		fprintf(stderr, "FAIL: attach gave err(%d), expected -EIO\n", err);
		return 1;
	}
	if (imp.nbdfs != 1) {
		fprintf(stderr, "FAIL: a failed attach left nbdfs(%u)\n", imp.nbdfs);
		return 1;
	}

	for (int i = 0; i < 2; ++i) {
		dmamem_iommu_map_pa_release(&imp, &attach[i]);
	}
	if (g_stale_del) {
		fprintf(stderr,
			"FAIL: release deleted %d handle(s) the failed attach had undone\n",
			g_stale_del);
		return 1;
	}
	if (live_handles()) {
		fprintf(stderr, "FAIL: %d handle(s) still mapped after release\n", live_handles());
		return 1;
	}
	if (g_nalloc) {
		fprintf(stderr, "FAIL: %ld allocation(s) left after release\n", g_nalloc);
		return 1;
	}

	printf("OK: a failed attach leaves nothing for release to delete twice\n");
	return 0;
}

int
main(void)
{
	int nerr = 0;

	nerr += test_populate_unwinds();
	nerr += test_attach_unwinds();

	return nerr ? 1 : 0;
}
