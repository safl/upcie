// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
//
// SPDX-License-Identifier: BSD-3-Clause
//
// Can a second process open a vfio cdev already held by another, bind it to
// its own iommufd, map memory, and reach the BAR?
//
//   vfio_cdev_probe <cdev> <attach:0|1> <hold-seconds>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/vfio.h>
#include <linux/iommufd.h>

#define STEP(label, expr)                                                   \
	do {                                                                \
		long _rc = (long)(expr);                                    \
		printf("  %-28s %s%s%s\n", label, _rc < 0 ? "FAIL " : "ok", \
		       _rc < 0 ? strerror(errno) : "", _rc < 0 ? "" : "");  \
	} while (0)

int
main(int argc, char **argv)
{
	const char *cdev = argc > 1 ? argv[1] : "/dev/vfio/devices/vfio0";
	int do_attach = argc > 2 ? atoi(argv[2]) : 1;
	int hold = argc > 3 ? atoi(argv[3]) : 0;
	struct vfio_device_bind_iommufd bind = {.argsz = sizeof(bind)};
	struct iommu_ioas_alloc alloc = {.size = sizeof(alloc)};
	struct iommu_ioas_map map = {.size = sizeof(map)};
	struct vfio_region_info reg = {.argsz = sizeof(reg)};
	int dfd, ifd, rc;
	void *buf, *bar;

	printf("[pid %d] %s attach=%d\n", getpid(), cdev, do_attach);

	dfd = open(cdev, O_RDWR);
	printf("  %-28s %s\n", "open(cdev)", dfd < 0 ? strerror(errno) : "ok");
	if (dfd < 0) {
		return 1;
	}

	ifd = open("/dev/iommu", O_RDWR);
	printf("  %-28s %s\n", "open(/dev/iommu)", ifd < 0 ? strerror(errno) : "ok");
	if (ifd < 0) {
		return 1;
	}

	bind.iommufd = ifd;
	rc = ioctl(dfd, VFIO_DEVICE_BIND_IOMMUFD, &bind);
	printf("  %-28s %s\n", "BIND_IOMMUFD", rc < 0 ? strerror(errno) : "ok");

	rc = ioctl(ifd, IOMMU_IOAS_ALLOC, &alloc);
	printf("  %-28s %s\n", "IOAS_ALLOC", rc < 0 ? strerror(errno) : "ok");

	if (do_attach) {
		struct vfio_device_attach_iommufd_pt att = {.argsz = sizeof(att)};

		att.pt_id = alloc.out_ioas_id;
		rc = ioctl(dfd, VFIO_DEVICE_ATTACH_IOMMUFD_PT, &att);
		printf("  %-28s %s\n", "ATTACH_IOMMUFD_PT", rc < 0 ? strerror(errno) : "ok");
	}

	buf = mmap(NULL, 2 << 20, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
	if (buf == MAP_FAILED) {
		buf = mmap(NULL, 2 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1,
			   0);
	}
	map.ioas_id = alloc.out_ioas_id;
	map.user_va = (unsigned long long)buf;
	map.length = 2 << 20;
	map.flags = IOMMU_IOAS_MAP_READABLE | IOMMU_IOAS_MAP_WRITEABLE;
	rc = ioctl(ifd, IOMMU_IOAS_MAP, &map);
	printf("  %-28s %s", "IOAS_MAP (2MiB)", rc < 0 ? strerror(errno) : "ok");
	if (rc == 0) {
		printf(" iova=0x%llx", (unsigned long long)map.iova);
	}
	printf("\n");

	reg.index = VFIO_PCI_BAR0_REGION_INDEX;
	rc = ioctl(dfd, VFIO_DEVICE_GET_REGION_INFO, &reg);
	printf("  %-28s %s\n", "GET_REGION_INFO(BAR0)", rc < 0 ? strerror(errno) : "ok");
	if (rc == 0) {
		bar = mmap(NULL, reg.size, PROT_READ | PROT_WRITE, MAP_SHARED, dfd, reg.offset);
		printf("  %-28s %s\n", "mmap(BAR0)", bar == MAP_FAILED ? strerror(errno) : "ok");
		if (bar != MAP_FAILED) {
			printf("  %-28s 0x%08x\n", "BAR0 dword0", *(volatile unsigned int *)bar);
		}
	}

	if (hold) {
		printf("  holding for %ds\n", hold);
		fflush(stdout);
		sleep(hold);
	}

	return 0;
}
