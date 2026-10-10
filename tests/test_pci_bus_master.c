// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * Turn bus mastering off and on for a PCI function through sysfs, reading the command register
 * back each time; needs root, works whichever driver is bound, and leaves bus mastering as it was.
 * Use an idle device: DMA in flight while bus mastering is off is lost.
 *
 *   test_pci_bus_master 0000:01:00.0
 */
#include <upcie/upcie.h>

static int
command(const char *bdf, uint16_t *cmd)
{
	char path[256];
	int fd, err = 0;

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/config", bdf);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		return -errno;
	}
	if (pread(fd, cmd, sizeof(*cmd), PCI_COMMAND) != sizeof(*cmd)) {
		err = -EIO;
	}
	close(fd);
	return err;
}

int
main(int argc, char **argv)
{
	uint16_t cmd = 0, before = 0;
	int err;

	if (argc < 2) {
		printf("usage: %s dddd:BB:DD.FF\n", argv[0]);
		return EINVAL;
	}
	if (command(argv[1], &before)) {
		fprintf(stderr, "FAIL: reading %s's command register\n", argv[1]);
		return 1;
	}

	for (int i = 0; i < 2; ++i) {
		int on = !!(before & PCI_COMMAND_MASTER), enable = i ? on : !on; /* flip, then back */

		err = pci_func_bus_master(argv[1], enable);
		if (err || command(argv[1], &cmd) || !!(cmd & PCI_COMMAND_MASTER) != enable) {
			fprintf(stderr, "FAIL: pci_func_bus_master(%d) err(%d), command 0x%04x\n",
				enable, err, cmd);
			return 1;
		}
		printf("bus master %s: command 0x%04x\n", enable ? "on" : "off", cmd);
	}

	return cmd == before ? 0 : 1;
}
