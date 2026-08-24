// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * Delegation probe: registration direction, and what survives the primary
 * =======================================================================
 *
 * Two questions a design for sharing a controller has to answer, and neither
 * is about GPUs, so neither needs one.
 *
 * The first is which way registration goes. A secondary holding the iommufd
 * can map its own memory, which charges the pinned pages to the secondary; a
 * secondary that only asks the primary to map a descriptor it sends charges
 * them to the primary. Both are done here, in that order, so that lowering
 * RLIMIT_MEMLOCK on either side shows which one it bounds.
 *
 * The second is what a secondary still has when the primary is gone. The
 * primary exits after serving, and the secondary then re-reads a register
 * through its own mapping and tries another mapping through the iommufd it was
 * handed. A device fd keeps the device bound; whether the address space
 * outlives the process that created it is the question the restart policy
 * turns on.
 *
 * Usage:
 *   upcie_vfio_delegate_probe primary <cdev> <socket>
 *   upcie_vfio_delegate_probe secondary <socket>
 *
 * Run the primary in the background; it serves one secondary and exits. Lower
 * the limit on either side to see the accounting move, for example with
 * "sh -c 'ulimit -l 64; ... secondary ...'".
 *
 * @file upcie_vfio_delegate_probe.c
 */

#include <upcie/upcie.h>

#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#define PROBE_NBYTES (2 << 20)

static void
say(const char *who, const char *what, const char *how)
{
	printf("  [%s] %-30s %s\n", who, what, how);
}

static void
say_limit(const char *who)
{
	struct rlimit rl = {0};
	char note[64];

	if (getrlimit(RLIMIT_MEMLOCK, &rl)) {
		return;
	}
	if (rl.rlim_cur == RLIM_INFINITY) {
		snprintf(note, sizeof(note), "unlimited");
	} else {
		snprintf(note, sizeof(note), "%llu bytes", (unsigned long long)rl.rlim_cur);
	}
	say(who, "RLIMIT_MEMLOCK", note);
}

static int
send_fd(int sock, int fd, uint64_t val)
{
	char control[CMSG_SPACE(sizeof(int))] = {0};
	struct iovec iov = {.iov_base = &val, .iov_len = sizeof(val)};
	struct msghdr msg = {0};
	struct cmsghdr *cmsg;

	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	if (fd >= 0) {
		msg.msg_control = control;
		msg.msg_controllen = sizeof(control);
		cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
	}

	return sendmsg(sock, &msg, 0) < 0 ? -errno : 0;
}

static int
recv_fd(int sock, int *fd, uint64_t *val)
{
	char control[CMSG_SPACE(sizeof(int))] = {0};
	struct iovec iov = {.iov_base = val, .iov_len = sizeof(*val)};
	struct msghdr msg = {0};
	struct cmsghdr *cmsg;
	ssize_t nbytes;

	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = control;
	msg.msg_controllen = sizeof(control);

	nbytes = recvmsg(sock, &msg, 0);
	if (nbytes < 0) {
		return -errno;
	}
	if (nbytes == 0) {
		return -ENODATA;
	}

	if (fd) {
		*fd = -1;
		cmsg = CMSG_FIRSTHDR(&msg);
		if (cmsg && cmsg->cmsg_type == SCM_RIGHTS) {
			memcpy(fd, CMSG_DATA(cmsg), sizeof(int));
		}
	}

	return 0;
}

static int
memfd_of(size_t nbytes)
{
	int fd = memfd_create("delegate_probe", 0);

	if (fd < 0) {
		return -errno;
	}
	if (ftruncate(fd, (off_t)nbytes)) {
		close(fd);
		return -errno;
	}

	return fd;
}

static int
secondary(const char *path)
{
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	struct vfio_region_info region = {.index = 0};
	struct iommufd iommufd = {0};
	uint64_t iova = 0, ignored = 0;
	int devfd = -1, iommu_fd = -1, memfd = -1;
	void *bar0;
	int sock;
	int err;

	say_limit("secondary");

	sock = socket(AF_UNIX, SOCK_STREAM, 0);
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	if (sock < 0 || connect(sock, (struct sockaddr *)&addr, sizeof(addr))) {
		say("secondary", "connect()", strerror(errno));
		return -errno;
	}

	if (recv_fd(sock, &devfd, &ignored) || devfd < 0) {
		say("secondary", "recv device fd", "FAILED");
		return -EPROTO;
	}
	if (recv_fd(sock, &iommu_fd, &ignored) || iommu_fd < 0) {
		say("secondary", "recv iommufd", "FAILED");
		return -EPROTO;
	}
	say("secondary", "recv device fd and iommufd", "ok");

	if (vfio_device_get_region_info(devfd, &region)) {
		say("secondary", "GET_REGION_INFO(BAR0)", strerror(errno));
		return -errno;
	}
	bar0 = vfio_map_region(devfd, region.size, region.offset);
	if (bar0 == MAP_FAILED) {
		say("secondary", "mmap(BAR0)", strerror(errno));
		return -errno;
	}
	printf("  [secondary] %-30s 0x%08" PRIx32 "\n", "host read", mmio_read32(bar0, 0));

	/* Direction one: the secondary maps its own memory through the passed
	 * iommufd, which is what measurement 2 did. */
	iommufd.fd = iommu_fd;
	iommufd.ioas_id = (uint32_t)ignored;
	memfd = memfd_of(PROBE_NBYTES);
	if (memfd < 0) {
		say("secondary", "memfd_create()", strerror(-memfd));
		return memfd;
	}

	err = iommufd_ioas_map_file(&iommufd, memfd, 0, PROBE_NBYTES,
				    IOMMU_IOAS_MAP_READABLE | IOMMU_IOAS_MAP_WRITEABLE, &iova);
	if (err) {
		say("secondary", "self map, charged to it", strerror(-err));
	} else {
		printf("  [secondary] %-30s ok, iova=0x%" PRIx64 "\n", "self map, charged to it",
		       iova);
	}

	/* And the same memory as an anonymous mapping rather than a
	 * descriptor, since the two are accounted differently and only this
	 * one is what measurement 2 exercised. */
	{
		void *anon = mmap(NULL, PROBE_NBYTES, PROT_READ | PROT_WRITE,
				  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		uint64_t anon_iova = 0;

		if (anon == MAP_FAILED) {
			say("secondary", "mmap(anonymous)", strerror(errno));
		} else {
			memset(anon, 0, PROBE_NBYTES);
			err = iommufd_ioas_map(&iommufd, (uint64_t)anon, PROBE_NBYTES,
					       IOMMU_IOAS_MAP_READABLE | IOMMU_IOAS_MAP_WRITEABLE,
					       &anon_iova);
			if (err) {
				say("secondary", "self map, anonymous", strerror(-err));
			} else {
				printf("  [secondary] %-30s ok, iova=0x%" PRIx64 "\n",
				       "self map, anonymous", anon_iova);
			}
		}
	}

	/* Direction two: hand the descriptor over and let the primary map it,
	 * which is what the design proposes. */
	err = send_fd(sock, memfd, PROBE_NBYTES);
	if (err) {
		say("secondary", "send memfd to primary", strerror(-err));
		return err;
	}
	if (recv_fd(sock, NULL, &iova)) {
		say("secondary", "primary maps it", "no answer");
	} else if (!iova) {
		say("secondary", "primary maps it", "refused");
	} else {
		printf("  [secondary] %-30s ok, iova=0x%" PRIx64 "\n", "primary maps it", iova);
	}

	/* The primary exits here; wait for it rather than racing. */
	while (recv_fd(sock, NULL, &ignored) != -ENODATA) {
	}
	say("secondary", "primary is gone", "ok");

	printf("  [secondary] %-30s 0x%08" PRIx32 "\n", "host read after", mmio_read32(bar0, 0));

	close(memfd);
	memfd = memfd_of(PROBE_NBYTES);
	iova = 0;
	err = iommufd_ioas_map_file(&iommufd, memfd, 0, PROBE_NBYTES,
				    IOMMU_IOAS_MAP_READABLE | IOMMU_IOAS_MAP_WRITEABLE, &iova);
	if (err) {
		say("secondary", "map after, via passed fd", strerror(-err));
	} else {
		printf("  [secondary] %-30s ok, iova=0x%" PRIx64 "\n", "map after, via passed fd",
		       iova);
	}

	return 0;
}

static int
primary(const char *cdev, const char *path)
{
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	struct iommufd iommufd = {0};
	struct vfio_cdev dev = {0};
	uint64_t iova = 0, nbytes = 0;
	int listener, sock, memfd = -1;
	int err;

	say_limit("primary");

	err = iommufd_open(&iommufd);
	if (!err) {
		err = iommufd_ioas_alloc(&iommufd);
	}
	if (!err) {
		err = vfio_cdev_open(cdev, &dev);
	}
	if (!err) {
		err = iommufd_bind(&iommufd, &dev);
	}
	if (!err) {
		err = iommufd_attach(&iommufd, &dev);
	}
	if (err) {
		say("primary", "device setup", strerror(-err));
		return err;
	}
	say("primary", "bind and attach", "ok");

	unlink(path);
	listener = socket(AF_UNIX, SOCK_STREAM, 0);
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	if (listener < 0 || bind(listener, (struct sockaddr *)&addr, sizeof(addr)) ||
	    listen(listener, 1) || chmod(path, 0666)) {
		say("primary", "listen()", strerror(errno));
		return -errno;
	}
	say("primary", "listening", path);

	sock = accept(listener, NULL, NULL);
	if (sock < 0) {
		say("primary", "accept()", strerror(errno));
		return -errno;
	}

	if (send_fd(sock, dev.fd, 0) || send_fd(sock, iommufd.fd, iommufd.ioas_id)) {
		say("primary", "send descriptors", "FAILED");
		return -EIO;
	}
	say("primary", "send device fd and iommufd", "ok");

	if (recv_fd(sock, &memfd, &nbytes) || memfd < 0) {
		say("primary", "recv memfd", "FAILED");
		return -EPROTO;
	}

	err = iommufd_ioas_map_file(&iommufd, memfd, 0, nbytes,
				    IOMMU_IOAS_MAP_READABLE | IOMMU_IOAS_MAP_WRITEABLE, &iova);
	if (err) {
		say("primary", "map on its behalf", strerror(-err));
		iova = 0;
	} else {
		printf("  [primary]   %-30s ok, iova=0x%" PRIx64 "\n", "map on its behalf", iova);
	}
	send_fd(sock, -1, iova);

	say("primary", "exiting", "ok");
	close(memfd);
	close(sock);
	close(listener);
	unlink(path);
	iommufd_detach(&dev);
	vfio_cdev_close(&dev);
	iommufd_close(&iommufd);

	return 0;
}

int
main(int argc, char *argv[])
{
	setvbuf(stdout, NULL, _IOLBF, 0);

	if (argc == 4 && !strcmp(argv[1], "primary")) {
		return primary(argv[2], argv[3]) ? EXIT_FAILURE : EXIT_SUCCESS;
	}
	if (argc == 3 && !strcmp(argv[1], "secondary")) {
		return secondary(argv[2]) ? EXIT_FAILURE : EXIT_SUCCESS;
	}

	fprintf(stderr, "usage: %s primary <cdev> <socket>\n", argv[0]);
	fprintf(stderr, "       %s secondary <socket>\n", argv[0]);

	return 2;
}
