// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
//
// SPDX-License-Identifier: BSD-3-Clause
//
// Can a privileged primary hand a vfio-pci device to an unprivileged secondary?
//
//   vfio_share primary   <cdev> <sock>
//   vfio_share secondary <sock>
//
// The primary binds the device, maps a memfd into its IOAS, then passes the
// device fd, the iommufd and the memfd over a unix socket. The secondary, which
// need not be root, tries to reach the BAR and register memory of its own.

// syscall(2) and ftruncate(2) are not declared under -std=c11 alone, and this
// probe deliberately includes no uPCIe header, since what it asks about is raw
// kernel behaviour.
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>
#include <linux/vfio.h>
#include <linux/iommufd.h>

#define R(l, rc) printf("  %-30s %s\n", l, (long)(rc) < 0 ? strerror(errno) : "ok")

static int
send_fds(int sock, int *fds, int n)
{
	struct msghdr msg = {0};
	struct iovec io = {.iov_base = "x", .iov_len = 1};
	char buf[CMSG_SPACE(sizeof(int) * 4)] = {0};
	struct cmsghdr *cm;

	msg.msg_iov = &io;
	msg.msg_iovlen = 1;
	msg.msg_control = buf;
	msg.msg_controllen = CMSG_SPACE(sizeof(int) * n);
	cm = CMSG_FIRSTHDR(&msg);
	cm->cmsg_level = SOL_SOCKET;
	cm->cmsg_type = SCM_RIGHTS;
	cm->cmsg_len = CMSG_LEN(sizeof(int) * n);
	memcpy(CMSG_DATA(cm), fds, sizeof(int) * n);

	return sendmsg(sock, &msg, 0);
}

static int
recv_fds(int sock, int *fds, int n)
{
	struct msghdr msg = {0};
	struct iovec io = {0};
	char c, buf[CMSG_SPACE(sizeof(int) * 4)] = {0};
	struct cmsghdr *cm;

	io.iov_base = &c;
	io.iov_len = 1;
	msg.msg_iov = &io;
	msg.msg_iovlen = 1;
	msg.msg_control = buf;
	msg.msg_controllen = sizeof(buf);
	if (recvmsg(sock, &msg, 0) < 0) {
		return -1;
	}
	cm = CMSG_FIRSTHDR(&msg);
	if (!cm) {
		return -1;
	}
	memcpy(fds, CMSG_DATA(cm), sizeof(int) * n);

	return 0;
}

static int
bar_probe(int dfd, const char *who)
{
	struct vfio_region_info reg = {.argsz = sizeof(reg)};
	void *bar;
	int rc;

	reg.index = VFIO_PCI_BAR0_REGION_INDEX;
	rc = ioctl(dfd, VFIO_DEVICE_GET_REGION_INFO, &reg);
	R("GET_REGION_INFO(BAR0)", rc);
	if (rc < 0) {
		return -1;
	}
	bar = mmap(NULL, reg.size, PROT_READ | PROT_WRITE, MAP_SHARED, dfd, reg.offset);
	R("mmap(BAR0)", bar == MAP_FAILED ? -1 : 0);
	if (bar == MAP_FAILED) {
		return -1;
	}
	printf("  %-30s 0x%08x   (%s)\n", "NVMe CAP low dword", *(volatile unsigned int *)bar,
	       who);

	return 0;
}

/*
 * Map a memfd into the IOAS, and say which way it was done.
 *
 * IOMMU_IOAS_MAP_FILE arrived in Linux 6.14, and distributions still shipping
 * older UAPI headers cannot see it at build time even where the running kernel
 * has it. Falling back to IOMMU_IOAS_MAP over a host mapping of the same memfd
 * keeps the question this probe asks answerable there.
 */
#ifdef IOMMU_IOAS_MAP_FILE
#define IOAS_MAP_MEMFD "IOAS_MAP_FILE(memfd)"
static int
ioas_map_memfd(int ifd, unsigned int ioas_id, int mfd, unsigned long long *iova)
{
	struct iommu_ioas_map_file mf = {.size = sizeof(mf)};

	mf.ioas_id = ioas_id;
	mf.fd = mfd;
	mf.start = 0;
	mf.length = 2 << 20;
	mf.flags = IOMMU_IOAS_MAP_READABLE | IOMMU_IOAS_MAP_WRITEABLE;
	if (ioctl(ifd, IOMMU_IOAS_MAP_FILE, &mf) == -1) {
		return -1;
	}
	*iova = mf.iova;

	return 0;
}
#else
#define IOAS_MAP_MEMFD "IOAS_MAP(memfd via user_va)"
static int
ioas_map_memfd(int ifd, unsigned int ioas_id, int mfd, unsigned long long *iova)
{
	struct iommu_ioas_map m = {.size = sizeof(m)};
	void *va = mmap(NULL, 2 << 20, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);

	if (va == MAP_FAILED) {
		return -1;
	}
	m.ioas_id = ioas_id;
	m.user_va = (unsigned long long)va;
	m.length = 2 << 20;
	m.flags = IOMMU_IOAS_MAP_READABLE | IOMMU_IOAS_MAP_WRITEABLE;
	if (ioctl(ifd, IOMMU_IOAS_MAP, &m) == -1) {
		return -1;
	}
	*iova = m.iova;

	return 0;
}
#endif

int
main(int argc, char **argv)
{
	const char *role = argc > 1 ? argv[1] : "";
	struct sockaddr_un sa = {.sun_family = AF_UNIX};
	int sock, fds[3];

	if (!strcmp(role, "primary")) {
		const char *cdev = argv[2];
		struct vfio_device_bind_iommufd bnd = {.argsz = sizeof(bnd)};
		struct iommu_ioas_alloc alloc = {.size = sizeof(alloc)};
		struct vfio_device_attach_iommufd_pt att = {.argsz = sizeof(att)};
		unsigned long long iova = 0;
		int dfd, ifd, mfd, cl;

		printf("[primary uid=%d]\n", getuid());
		dfd = open(cdev, O_RDWR);
		R("open(cdev)", dfd);
		ifd = open("/dev/iommu", O_RDWR);
		R("open(/dev/iommu)", ifd);
		bnd.iommufd = ifd;
		R("BIND_IOMMUFD", ioctl(dfd, VFIO_DEVICE_BIND_IOMMUFD, &bnd));
		R("IOAS_ALLOC", ioctl(ifd, IOMMU_IOAS_ALLOC, &alloc));
		printf("  %-30s %u\n", "ioas_id", alloc.out_ioas_id);
		fflush(stdout);
		att.pt_id = alloc.out_ioas_id;
		R("ATTACH_IOMMUFD_PT", ioctl(dfd, VFIO_DEVICE_ATTACH_IOMMUFD_PT, &att));

		mfd = syscall(SYS_memfd_create, "dma", 0);
		R("memfd_create", mfd);
		R("ftruncate(2MiB)", ftruncate(mfd, 2 << 20));
		R(IOAS_MAP_MEMFD, ioas_map_memfd(ifd, alloc.out_ioas_id, mfd, &iova));
		printf("  %-30s 0x%llx\n", "  -> iova", iova);
		bar_probe(dfd, "primary");

		unlink(argv[3]);
		sock = socket(AF_UNIX, SOCK_STREAM, 0);
		strncpy(sa.sun_path, argv[3], sizeof(sa.sun_path) - 1);
		(void)bind(sock, (struct sockaddr *)&sa, sizeof(sa));
		listen(sock, 1);
		chmod(argv[3], 0666);
		printf("  waiting for secondary...\n");
		fflush(stdout);
		cl = accept(sock, NULL, NULL);
		fds[0] = dfd;
		fds[1] = ifd;
		fds[2] = mfd;
		R("send fds", send_fds(cl, fds, 3));
		fflush(stdout);
		sleep(8);

		return 0;
	}

	printf("[secondary uid=%d]\n", getuid());
	sock = socket(AF_UNIX, SOCK_STREAM, 0);
	strncpy(sa.sun_path, argv[2], sizeof(sa.sun_path) - 1);
	R("connect", connect(sock, (struct sockaddr *)&sa, sizeof(sa)));
	R("recv fds", recv_fds(sock, fds, 3));
	printf("  device fd=%d iommufd=%d memfd=%d\n", fds[0], fds[1], fds[2]);

	bar_probe(fds[0], "secondary");

	{
		void *p = mmap(NULL, 2 << 20, PROT_READ | PROT_WRITE, MAP_SHARED, fds[2], 0);
		R("mmap(shared memfd)", p == MAP_FAILED ? -1 : 0);
		if (p != MAP_FAILED) {
			*(volatile unsigned int *)p = 0xC0FFEE;
			printf("  %-30s wrote 0x%08x\n", "shared DMA buffer",
			       *(volatile unsigned int *)p);
		}
	}
	{
		struct iommu_ioas_map m = {.size = sizeof(m)};
		void *own = mmap(NULL, 2 << 20, PROT_READ | PROT_WRITE,
				 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

		m.ioas_id = argc > 3 ? (unsigned)atoi(argv[3]) : 0;
		m.user_va = (unsigned long long)own;
		m.length = 2 << 20;
		m.flags = IOMMU_IOAS_MAP_READABLE | IOMMU_IOAS_MAP_WRITEABLE;
		R("IOAS_MAP own buffer", ioctl(fds[1], IOMMU_IOAS_MAP, &m));
		if (m.iova) {
			printf("  %-30s 0x%llx\n", "  -> iova", (unsigned long long)m.iova);
		}
	}

	return 0;
}
