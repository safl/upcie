/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * What the driver says it is holding, and where
 *
 * Deciding where an imported dma-buf lives by looking at its addresses is
 * delicate: an IOMMU turns them into IOVAs and the comparison stops meaning
 * anything. The driver's own accounting does not have that problem. amdgpu
 * publishes how much VRAM and how much GTT it is holding, so an allocation
 * migrating from one to the other is visible as it happens, in numbers nobody
 * had to interpret.
 *
 * Sample it around each step and the migration either shows up or it does not.
 * Use an allocation of several GiB so the change dwarfs whatever else the
 * machine is doing.
 *
 * @file dmabuf_import_accounting.h
 */
#ifndef DMABUF_IMPORT_ACCOUNTING_H
#define DMABUF_IMPORT_ACCOUNTING_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct gpu_accounting {
	uint64_t vram_used;
	uint64_t gtt_used;
	uint64_t host_available; ///< MemAvailable, in bytes
	int have_gpu;            ///< 0 when the driver publishes no accounting
};

static inline uint64_t
_read_u64(const char *path)
{
	unsigned long long val = 0;
	FILE *fh = fopen(path, "r");

	if (!fh) {
		return 0;
	}
	if (fscanf(fh, "%llu", &val) != 1) {
		val = 0;
	}
	fclose(fh);

	return val;
}

static inline uint64_t
_mem_available(void)
{
	unsigned long long kb = 0;
	char key[64];
	FILE *fh = fopen("/proc/meminfo", "r");

	if (!fh) {
		return 0;
	}
	while (fscanf(fh, "%63s %llu kB\n", key, &kb) == 2) {
		if (!strcmp(key, "MemAvailable:")) {
			fclose(fh);
			return kb * 1024ULL;
		}
	}
	fclose(fh);

	return 0;
}

/**
 * Sample the driver's accounting for `bdf`
 *
 * Only amdgpu publishes this; elsewhere `have_gpu` is 0 and the host figure is
 * still worth having, since memory the GPU stopped holding has to appear
 * somewhere.
 */
static inline struct gpu_accounting
gpu_accounting_read(const char *bdf)
{
	struct gpu_accounting acc;
	char path[256];

	memset(&acc, 0, sizeof(acc));
	acc.host_available = _mem_available();

	if (!bdf) {
		return acc;
	}

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/mem_info_vram_used", bdf);
	acc.vram_used = _read_u64(path);
	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/mem_info_gtt_used", bdf);
	acc.gtt_used = _read_u64(path);
	acc.have_gpu = acc.vram_used || acc.gtt_used;

	return acc;
}

static inline void
gpu_accounting_pr(const char *stage, const char *bdf, struct gpu_accounting base)
{
	struct gpu_accounting now = gpu_accounting_read(bdf);
	const double mib = 1024.0 * 1024.0;

	/* Signed, and computed after the conversion: these counters fall as well
	 * as rise, and an unsigned subtraction turns a decrease into a number
	 * with sixteen digits. */
	const double d_vram = ((double)now.vram_used - (double)base.vram_used) / mib;
	const double d_gtt = ((double)now.gtt_used - (double)base.gtt_used) / mib;
	const double d_host = ((double)now.host_available - (double)base.host_available) / mib;

	if (now.have_gpu) {
		printf("%-22s vram %8.1f MiB (%+9.1f)   gtt %8.1f MiB (%+9.1f)   "
		       "host avail %+9.1f MiB\n",
		       stage, now.vram_used / mib, d_vram, now.gtt_used / mib, d_gtt, d_host);
	} else {
		printf("%-22s host avail %+9.1f MiB (driver publishes no accounting)\n", stage,
		       d_host);
	}
}

#endif /* DMABUF_IMPORT_ACCOUNTING_H */
