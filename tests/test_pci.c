#include <hostmem.h>
#include <pci.h>

#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	struct pci_func func = {0};
	int err;

	if (argc < 2) {
		printf("usage: %s dddd:BB:DD.FF\n", argv[0]);
		return EINVAL;
	}

	err = pci_func_open(argv[1], &func);
	if (err) {
		perror("pci_function_open");
		return -err;
	}

	for (int id = 0; id < PCI_NBARS; ++id) {
		err = pci_bar_map(func.bdf, id, &func.bars[id]);
		if (err && (errno != ENOENT)) {
			perror("pci_bar_map");
			return errno;
		}

		pci_bar_pr(&func.bars[id]);
	}

	pci_func_close(&func);

	return 0;
}
