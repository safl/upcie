// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * Resize a BAR to the largest size the function offers, and check that sysfs reports that size
 *
 * The function must have no driver bound, as the kernel requires for a resize:
 *
 *   test_pci_bar_resize 0000:08:00.0 1
 */
#include <upcie/upcie.h>

int
main(int argc, char **argv)
{
	size_t before = 0, after = 0, now = 0;
	uint64_t sizes = 0;
	uint8_t id;
	int err;

	if (argc < 3) {
		printf("usage: %s dddd:BB:DD.FF BAR\n", argv[0]);
		return EINVAL;
	}
	id = (uint8_t)atoi(argv[2]);

	err = pci_bar_resize_sizes(argv[1], id, &sizes);
	if (err) {
		fprintf(stderr, "FAIL: pci_bar_resize_sizes() err(%d): %s\n", err, strerror(-err));
		return -err;
	}
	err = pci_bar_size(argv[1], id, &before);
	if (err) {
		fprintf(stderr, "FAIL: pci_bar_size() err(%d): %s\n", err, strerror(-err));
		return -err;
	}
	printf("BAR%u: %zu MiB, offers 0x%" PRIx64 " (bit n: 2^n MiB)\n", id, before >> 20, sizes);

	err = pci_bar_resize(argv[1], id, (1ULL << 20) + 1);
	if (err != -EINVAL) {
		fprintf(stderr, "FAIL: a size that is no power of two gave err(%d)\n", err);
		return 1;
	}

	err = pci_bar_resize_largest(argv[1], id, &after);
	if (err) {
		fprintf(stderr, "FAIL: pci_bar_resize_largest() err(%d): %s\n", err, strerror(-err));
		return -err;
	}
	err = pci_bar_size(argv[1], id, &now);
	if (err || now != after || after >> 20 != 1ULL << (63 - __builtin_clzll(sizes))) {
		fprintf(stderr, "FAIL: BAR%u is %zu MiB, the largest offered is %llu MiB\n", id,
			now >> 20, 1ULL << (63 - __builtin_clzll(sizes)));
		return 1;
	}

	printf("BAR%u: %zu MiB\n", id, after >> 20);
	return 0;
}
