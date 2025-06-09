/**
 * Helpers making use of mapping BAR regions via /sys/bus/pci/devices/<PCI_ADDR>/resourceX
 *
 * @file pci.h
 */
#ifndef PCI_H
#define PCI_H

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define PCI_BDF_LEN 12
#define PCI_NBARS 6

/**
 * Representation of a bar-address
 */
struct pci_addr {
	uint16_t domain;
	uint8_t bus;
	uint8_t device;
	uint8_t function;
};

/**
 * Encapsulation of a PCI BAR region mapping
 *
 * Add bar-accesors casting the access to 'volatile'
 */
struct pci_func_bar {
	uint64_t size;	 ///< The size of the BAR region
	uint8_t *region; ///< Pointer to mmap'ed BAR region
	uint8_t id;	 ///< One of the six BARs; [0-5]
	int fd;		 ///< Handle to file-representation
};

struct pci_func {
	struct pci_addr addr;	   ///< The address of the PCI device function
	char bdf[PCI_BDF_LEN + 1]; ///< PCI address as a nul-string full-BDF; e.g. "0000:05:00.0"
	struct pci_func_bar bars[PCI_NBARS]; ///< The six BARs associated with a PCI Function
};

static inline int
pci_bar_pr(struct pci_func_bar *bar)
{
	int wrtn = 0;

	printf("pci_bar:\n");
	printf("  id: %" PRIu8 "\n", bar->id);
	printf("  fd: %" PRIu8 "\n", bar->fd);
	printf("  size: %" PRIu64 "\n", bar->size);
	printf("  region: %p\n", bar->region);

	return wrtn;
};

/**
 * Fills the given 'addr' with parts found when scanning a text repr. on the form '0000:05:00.0'
 */
static inline int
pci_addr_from_text(const char *text, struct pci_addr *addr)
{
	int domain, bus, device, function;
	int fields;

	fields = sscanf(text, "%4x:%2x:%2x.%1x", &domain, &bus, &device, &function);
	if (fields != 4) {
		return -EINVAL;
	}

	if ((domain > 0xFFFF) || (bus > 0xFF) || (device > 0x1F) || (function > 0x07)) {
		return -EINVAL;
	}

	addr->domain = domain;
	addr->bus = bus;
	addr->device = device;
	addr->function = function;

	return 0;
}

/**
 * Populates the given char-array with a textual representation of the given 'addr'
 */
static inline int
pci_addr_to_text(struct pci_addr *addr, char *text)
{
	sprintf(text, "%" PRIu16 ":%" PRIu8 ":%" PRIu8 ".%" PRIu8, addr->domain, addr->bus,
		addr->device, addr->function);

	return 0;
}

static inline int
pci_func_open(const char *bdf, struct pci_func *func)
{
	int err;

	err = pci_addr_from_text(bdf, &func->addr);
	if (err) {
		return err;
	}
	sprintf(func->bdf, "%.*s", PCI_BDF_LEN, bdf);

	for (int id = 0; id < PCI_NBARS; ++id) {
		func->bars[id].id = id;
	}

	return 0;
}

static inline int
pci_bar_unmap(struct pci_func_bar *bar)
{
	if (!bar) {
		return -EINVAL;
	}

	if (bar->region) {
		munmap(bar->region, bar->size);
		close(bar->fd);
	}

	return 0;
}

/**
 * TODO: Add helpers reading / writing the bar, that is something which explicitly casts the access
 * to volatile to ensure the access is not optimized away by the compiler.
 */
static inline int
pci_bar_map(const char *bdf, uint8_t id, struct pci_func_bar *bar)
{
	struct stat barstat = {0};
	char path[256] = {0};
	int err;

	sprintf(path, "/sys/bus/pci/devices/%.*s/resource%" PRIu8, PCI_BDF_LEN, bdf, id);

	err = stat(path, &barstat);
	if (err) {
		return -errno;
	}

	bar->fd = open(path, O_RDWR);
	if (bar->fd == -1) {
		return -errno;
	}

	bar->size = barstat.st_size;
	bar->id = id;
	bar->region = mmap(NULL, bar->size, PROT_READ | PROT_WRITE, MAP_SHARED, bar->fd, 0x0);
	if (MAP_FAILED == bar->region) {
		close(bar->fd);
		return -errno;
	}

	return 0;
}

static inline void
pci_func_close(struct pci_func *func)
{
	if (!func) {
		return;
	}

	for (int id = 0; id < PCI_NBARS; ++id) {
		pci_bar_unmap(&func->bars[id]);
	}
}
#endif