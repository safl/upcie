// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * Turn bus mastering off and on for a vfio device, reading the command register back each time
 *
 * The device must be bound to vfio-pci; it is left with bus mastering on:
 *
 *   test_vfio_bus_master /dev/vfio/devices/vfio0
 */
#include <upcie/upcie.h>

static int
command(int fd, uint16_t *cmd)
{
	struct vfio_region_info config = {.index = VFIO_PCI_CONFIG_REGION_INDEX};

	if (vfio_device_get_region_info(fd, &config) < 0) {
		return -errno;
	}
	return pread(fd, cmd, sizeof(*cmd), config.offset + PCI_COMMAND) == sizeof(*cmd) ? 0 : -EIO;
}

int
main(int argc, char **argv)
{
	struct iommufd iommufd = {0};
	struct vfio_cdev dev = {0};
	uint16_t cmd = 0;
	int err, rc = 1;

	if (argc < 2) {
		printf("usage: %s /dev/vfio/devices/vfioN\n", argv[0]);
		return EINVAL;
	}

	err = iommufd_open(&iommufd);
	if (err) {
		fprintf(stderr, "FAIL: iommufd_open() err(%d): %s\n", err, strerror(-err));
		return 1;
	}
	err = vfio_cdev_open(argv[1], &dev);
	if (err) {
		fprintf(stderr, "FAIL: vfio_cdev_open('%s') err(%d): %s\n", argv[1], err,
			strerror(-err));
		goto out_iommufd;
	}
	err = iommufd_bind(&iommufd, &dev);
	if (err) {
		fprintf(stderr, "FAIL: iommufd_bind() err(%d): %s\n", err, strerror(-err));
		goto out_dev;
	}

	for (int enable = 0; enable <= 1; ++enable) {
		err = vfio_device_bus_master(dev.fd, enable);
		if (err || command(dev.fd, &cmd)) {
			fprintf(stderr, "FAIL: vfio_device_bus_master(%d) err(%d)\n", enable, err);
			goto out_dev;
		}
		if (!!(cmd & PCI_COMMAND_MASTER) != enable) {
			fprintf(stderr, "FAIL: command 0x%04x after vfio_device_bus_master(%d)\n", cmd,
				enable);
			goto out_dev;
		}
		printf("bus master %s: command 0x%04x\n", enable ? "on" : "off", cmd);
	}

	err = vfio_device_pci_command(dev.fd, PCI_COMMAND_MEMORY, 0);
	if (err || command(dev.fd, &cmd) || !(cmd & PCI_COMMAND_MEMORY)) {
		fprintf(stderr, "FAIL: vfio_device_pci_command(MEMORY) err(%d), command 0x%04x\n", err,
			cmd);
		goto out_dev;
	}
	rc = 0;

out_dev:
	vfio_cdev_close(&dev);
out_iommufd:
	iommufd_close(&iommufd);
	return rc;
}
