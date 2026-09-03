/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * Where did an imported dma-buf actually end up?
 *
 * An exporter may satisfy an import by migrating the buffer to system memory.
 * That succeeds, the addresses look plausible, and I/O through them works, so
 * nothing downstream notices it is no longer peer-to-peer. Deciding which
 * happened is harder than it looks, and two obvious checks are each wrong on
 * their own:
 *
 *   - DMABUF_IMPORT_GET_INFO reports segments carrying a PCI bus address, which
 *     the kernel sets only when its own P2PDMA framework did the mapping. That
 *     needs a PCI importer, and this module's is a misc device, so the count
 *     reads zero even for memory that plainly sits in the exporter's BAR. A
 *     false negative.
 *
 *   - Testing the address against the exporter's BAR is meaningful only while
 *     nothing translates it. Behind an IOMMU the importer sees IOVAs, and an
 *     IOVA may land in the BAR range by coincidence. A false positive.
 *
 * So this classifies an address against both the exporter's BAR ranges and the
 * kernel's System RAM ranges, and answers UNKNOWN rather than guessing when the
 * address belongs to neither or to both. The bus-address count is reported
 * alongside as corroboration, never as the verdict.
 *
 * @file dmabuf_import_placement.h
 */
#ifndef DMABUF_IMPORT_PLACEMENT_H
#define DMABUF_IMPORT_PLACEMENT_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#include "module/dmabuf_import.h"

enum dmabuf_placement {
	DMABUF_PLACEMENT_ERROR = -1,
	DMABUF_PLACEMENT_DEVICE = 0, ///< Every segment sits in the exporter's BAR
	DMABUF_PLACEMENT_HOST = 1,   ///< Every segment sits in system memory
	DMABUF_PLACEMENT_UNKNOWN = 2 ///< Neither, or a mix; no verdict is offered
};

struct dmabuf_range {
	uint64_t beg;
	uint64_t end; ///< Inclusive, as /proc/iomem and sysfs both report it
};

#define DMABUF_RANGES_MAX 64

/**
 * The exporter's memory BARs, from sysfs
 *
 * @return How many ranges were collected, or -1 when they cannot be read
 */
static inline int
dmabuf_bar_ranges(const char *bdf, struct dmabuf_range *out, int max)
{
	char path[256];
	FILE *fh;
	int n = 0;

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/resource", bdf);
	fh = fopen(path, "r");
	if (!fh) {
		return -1;
	}

	while (n < max) {
		unsigned long long beg, end, flags;

		if (fscanf(fh, "%llx %llx %llx", &beg, &end, &flags) != 3) {
			break;
		}
		/* Memory BARs only, and only those actually assigned. */
		if (!beg || end <= beg || !(flags & 0x200)) {
			continue;
		}
		out[n].beg = beg;
		out[n].end = end;
		++n;
	}
	fclose(fh);

	return n;
}

/**
 * The kernel's System RAM ranges, from /proc/iomem
 *
 * Reading these needs privilege; without it the addresses read as zero and no
 * verdict is possible, which is why the caller is told how many were found.
 */
static inline int
dmabuf_ram_ranges(struct dmabuf_range *out, int max)
{
	char line[512];
	FILE *fh;
	int n = 0;

	fh = fopen("/proc/iomem", "r");
	if (!fh) {
		return -1;
	}

	while ((n < max) && fgets(line, sizeof(line), fh)) {
		unsigned long long beg, end;
		char *sep;

		if (line[0] == ' ') {
			continue; /* A child range; only top-level ones matter. */
		}
		sep = strstr(line, " : ");
		if (!sep || strncmp(sep + 3, "System RAM", 10)) {
			continue;
		}
		if (sscanf(line, "%llx-%llx", &beg, &end) != 2) {
			continue;
		}
		if (!beg && !end) {
			continue; /* Unprivileged: the addresses are masked. */
		}
		out[n].beg = beg;
		out[n].end = end;
		++n;
	}
	fclose(fh);

	return n;
}

static inline int
dmabuf_range_holds(const struct dmabuf_range *ranges, int n, uint64_t addr)
{
	for (int i = 0; i < n; ++i) {
		if ((addr >= ranges[i].beg) && (addr <= ranges[i].end)) {
			return 1;
		}
	}

	return 0;
}

/**
 * Classify an already-attached import and print the verdict
 *
 * @param import_fd Open descriptor for the module
 * @param dmabuf_fd The dma-buf, already passed to DMABUF_IMPORT_ATTACH
 * @param bdf The exporting device, or NULL when it is not a PCI device
 * @param what Label for the printed line
 */
static inline enum dmabuf_placement
dmabuf_import_placement(int import_fd, int dmabuf_fd, const char *bdf, const char *what)
{
	struct dmabuf_range bars[DMABUF_RANGES_MAX], ram[DMABUF_RANGES_MAX];
	struct dmabuf_import_get_map *map;
	struct dmabuf_import_describe desc;
	struct dmabuf_import_info info;
	int nbars = 0, nram, ndev = 0, nhost = 0, desc_ok = 0;
	enum dmabuf_placement verdict;

	memset(&info, 0, sizeof(info));
	info.fd = dmabuf_fd;
	if (ioctl(import_fd, DMABUF_IMPORT_GET_INFO, &info) || !info.count) {
		printf("%-16s GET_INFO failed\n", what);
		return DMABUF_PLACEMENT_ERROR;
	}

	/* Every signal at once: no single one of them decides, and seeing them
	 * disagree is how an assumption gets caught. */
	{
		memset(&desc, 0, sizeof(desc));
		desc.fd = dmabuf_fd;
		desc_ok = !ioctl(import_fd, DMABUF_IMPORT_DESCRIBE, &desc);
		if (desc_ok) {
			printf("%-16s exporter %s, importer %s, %u segments, %.1f MiB,"
			       " %u on the bus, %u without a page, pinned %u\n",
			       what, desc.exporter, desc.importer, desc.count,
			       desc.nbytes / (1024.0 * 1024.0), desc.nbus, desc.nopage,
			       desc.pinned);
		}
	}

	nram = dmabuf_ram_ranges(ram, DMABUF_RANGES_MAX);
	if (bdf) {
		nbars = dmabuf_bar_ranges(bdf, bars, DMABUF_RANGES_MAX);
	}
	if ((nram <= 0) || (bdf && (nbars <= 0))) {
		/* The page count needs no ranges, so it can still answer. */
		if (desc_ok && desc.count && (desc.nopage == desc.count)) {
			printf("%-16s device memory: no page behind any of %u segments"
			       " (addresses not checked)\n",
			       what, desc.count);
			return DMABUF_PLACEMENT_DEVICE;
		}
		printf("%-16s no verdict: need /proc/iomem and the exporter's BARs\n", what);
		return DMABUF_PLACEMENT_UNKNOWN;
	}

	map = calloc(1, sizeof(*map) + info.count * sizeof(struct dmabuf_import_dma_map));
	if (!map) {
		return DMABUF_PLACEMENT_ERROR;
	}
	map->fd = dmabuf_fd;
	map->count = info.count;
	if (ioctl(import_fd, DMABUF_IMPORT_GET_MAP, map)) {
		printf("%-16s GET_MAP failed\n", what);
		free(map);
		return DMABUF_PLACEMENT_ERROR;
	}

	for (uint32_t i = 0; i < map->count; ++i) {
		uint64_t addr = map->dma_arr[i].dma_addr;

		if (nbars && dmabuf_range_holds(bars, nbars, addr)) {
			++ndev;
		} else if (dmabuf_range_holds(ram, nram, addr)) {
			++nhost;
		}
	}

	/* Segments with no struct page behind them are never host memory, and
	 * that holds where the addresses cannot be read: behind an IOMMU they
	 * are IOVAs and belong to no range worth comparing against. So this
	 * decides, and the addresses corroborate when they can. */
	if (desc_ok && desc.count && (desc.nopage == desc.count)) {
		verdict = DMABUF_PLACEMENT_DEVICE;
		printf("%-16s device memory: no page behind any of %u segments%s\n", what,
		       desc.count,
		       ndev == (int)map->count ? ", and every address is in the exporter's BAR"
					       : "");
	} else if (nhost == (int)map->count) {
		verdict = DMABUF_PLACEMENT_HOST;
		printf("%-16s host memory: %u/%u segments in system RAM\n", what, nhost,
		       map->count);
	} else if (ndev == (int)map->count) {
		verdict = DMABUF_PLACEMENT_DEVICE;
		printf("%-16s device memory: %u/%u segments in the exporter's BAR\n", what, ndev,
		       map->count);
	} else {
		verdict = DMABUF_PLACEMENT_UNKNOWN;
		printf("%-16s no verdict: %u/%u in the BAR, %u/%u in system RAM, %u/%u without"
		       " a page\n",
		       what, ndev, map->count, nhost, map->count, desc_ok ? desc.nopage : 0,
		       desc_ok ? desc.count : 0);
	}

	free(map);

	return verdict;
}

#endif /* DMABUF_IMPORT_PLACEMENT_H */
