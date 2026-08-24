// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * Delegated MMIO probe: can a secondary reach the BAR from a GPU kernel?
 * =====================================================================
 *
 * A process can pass a vfio device fd to an unrelated process over SCM_RIGHTS,
 * and the receiver can map BAR0 and read a register. What that leaves untested
 * is the step the GPU-initiated NVMe path depends on: registering a window of
 * that received mapping as I/O memory with cuMemHostRegister(), and reaching
 * it from an SM.
 *
 * Two processes rather than a fork with a setuid in it. Dropping privilege is
 * not the same state as never having had it: the dropped process keeps the
 * supplementary groups it was started with unless they are cleared, and a uid
 * change clears the dumpable flag, either of which could be what a refusal is
 * really about. So the secondary is started separately, as whatever user it is
 * started as, and the two meet over a named socket.
 *
 * It reads CAP rather than writing a doorbell, so nothing here disturbs a
 * controller, and both sides print what they read.
 *
 * Usage:
 *   upcie_vfio_share_gpu_probe_cuda primary <cdev> <socket>
 *   upcie_vfio_share_gpu_probe_cuda secondary <socket>
 *   upcie_vfio_share_gpu_probe_cuda standalone <cdev>
 *
 * The standalone mode is the control: no delegation, the process opens the
 * device itself. Run as an ordinary user it says whether a refusal is about
 * the caller or about the descriptor having been passed.
 *
 * The primary serves one secondary and exits when it disconnects, so run it in
 * the background and start the secondary as the user in question, for example
 * with setpriv --reuid=1000 --regid=1000 --clear-groups.
 *
 * @file upcie_vfio_share_gpu_probe_cuda.c
 */

/* upcie.h sets the feature-test macros, so it comes before anything that
 * pulls in features.h. */
#include <upcie/upcie_cuda.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

int
upcie_probe_read_mmio(const void *mmio, uint32_t *out);

static void
say(const char *who, const char *what, const char *how)
{
	printf("  [%s] %-28s %s\n", who, what, how);
}

static int
send_fd(int sock, int fd)
{
	char control[CMSG_SPACE(sizeof(int))] = {0};
	struct iovec iov = {.iov_base = (void *)"f", .iov_len = 1};
	struct msghdr msg = {0};
	struct cmsghdr *cmsg;

	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = control;
	msg.msg_controllen = sizeof(control);

	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

	return sendmsg(sock, &msg, 0) < 0 ? -errno : 0;
}

static int
recv_fd(int sock, int *fd)
{
	char control[CMSG_SPACE(sizeof(int))] = {0};
	char byte = 0;
	struct iovec iov = {.iov_base = &byte, .iov_len = 1};
	struct msghdr msg = {0};
	struct cmsghdr *cmsg;

	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = control;
	msg.msg_controllen = sizeof(control);

	if (recvmsg(sock, &msg, 0) < 0) {
		return -errno;
	}

	cmsg = CMSG_FIRSTHDR(&msg);
	if (!cmsg || cmsg->cmsg_type != SCM_RIGHTS) {
		return -EPROTO;
	}
	memcpy(fd, CMSG_DATA(cmsg), sizeof(int));

	return 0;
}

/**
 * What a process does with a device fd, however it came by one.
 */
static int
use_bar(const char *who, int fd, uint64_t offset)
{
	struct vfio_region_info region = {.index = 0};
	uint32_t gpu[2] = {0};
	CUcontext ctx;
	CUdevice cudev;
	void *bar0;
	int err;

	if (vfio_device_get_region_info(fd, &region)) {
		say(who, "GET_REGION_INFO(BAR0)", strerror(errno));
		return -errno;
	}

	bar0 = vfio_map_region(fd, region.size, region.offset);
	if (bar0 == MAP_FAILED) {
		say(who, "mmap(BAR0)", strerror(errno));
		return -errno;
	}
	printf("  [%s] %-28s 0x%08" PRIx32 " 0x%08" PRIx32 "\n", who, "host read",
	       mmio_read32(bar0, offset), mmio_read32(bar0, offset + 4));

	if (cuInit(0) || cuDeviceGet(&cudev, 0) || cuDevicePrimaryCtxRetain(&ctx, cudev) ||
	    cuCtxSetCurrent(ctx)) {
		say(who, "CUDA init", "FAILED");
		return -EIO;
	}

	/* The step this probe exists for: the shipping GPU-initiated path
	 * registers a doorbell the same way. */
	err = (int)cuMemHostRegister((uint8_t *)bar0 + offset, 2 * sizeof(uint32_t),
				     CU_MEMHOSTREGISTER_IOMEMORY);
	if (err) {
		const char *name = NULL;

		cuGetErrorName((CUresult)err, &name);
		say(who, "cuMemHostRegister(IOMEMORY)", name ? name : "FAILED");
		return -EIO;
	}
	say(who, "cuMemHostRegister(IOMEMORY)", "ok");

	err = upcie_probe_read_mmio((uint8_t *)bar0 + offset, gpu);
	if (err) {
		say(who, "kernel read", "FAILED");
		return -EIO;
	}
	printf("  [%s] %-28s 0x%08" PRIx32 " 0x%08" PRIx32 "\n", who, "kernel read", gpu[0],
	       gpu[1]);

	return 0;
}

static void
say_creds(const char *who)
{
	char note[96];

	snprintf(note, sizeof(note), "uid=%u euid=%u gid=%u", (unsigned)getuid(),
		 (unsigned)geteuid(), (unsigned)getgid());
	say(who, "running as", note);
}

/**
 * The secondary: a received descriptor and nothing else.
 */
static int
secondary(int sock, uint64_t offset)
{
	int fd = -1;
	int err;

	say_creds("secondary");

	err = recv_fd(sock, &fd);
	if (err) {
		say("secondary", "recv device fd", strerror(-err));
		return err;
	}
	say("secondary", "recv device fd", "ok");

	return use_bar("secondary", fd, offset);
}

/**
 * Standalone: no delegation at all, the process opens the device itself.
 *
 * This is the control for the secondary's result. If it is refused the same
 * way, the refusal is about the calling process rather than about the
 * descriptor having been passed to it. Needs the cdev and /dev/iommu to be
 * reachable by whoever runs it.
 */
static int
standalone(const char *cdev, uint64_t offset)
{
	struct iommufd iommufd = {0};
	struct vfio_cdev dev = {0};
	int err;

	say_creds("standalone");

	err = iommufd_open(&iommufd);
	if (err) {
		say("standalone", "iommufd_open()", strerror(-err));
		return err;
	}

	err = iommufd_ioas_alloc(&iommufd);
	if (err) {
		say("standalone", "iommufd_ioas_alloc()", strerror(-err));
		return err;
	}

	err = vfio_cdev_open(cdev, &dev);
	if (err) {
		say("standalone", "vfio_cdev_open()", strerror(-err));
		return err;
	}

	err = iommufd_bind(&iommufd, &dev);
	if (err) {
		say("standalone", "iommufd_bind()", strerror(-err));
		return err;
	}

	err = iommufd_attach(&iommufd, &dev);
	if (err) {
		say("standalone", "iommufd_attach()", strerror(-err));
		return err;
	}
	say("standalone", "bind and attach", "ok");

	return use_bar("standalone", dev.fd, offset);
}

static int
listen_at(const char *path)
{
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	int sock;

	unlink(path);

	sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock < 0) {
		return -errno;
	}

	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) || listen(sock, 1)) {
		close(sock);
		return -errno;
	}

	/* The secondary is another user; the socket has to be reachable by it. */
	if (chmod(path, 0666)) {
		close(sock);
		return -errno;
	}

	return sock;
}

static int
connect_to(const char *path)
{
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	int sock;

	sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock < 0) {
		return -errno;
	}

	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

	if (connect(sock, (struct sockaddr *)&addr, sizeof(addr))) {
		close(sock);
		return -errno;
	}

	return sock;
}

/**
 * The primary: holds the device, hands over the descriptor, serves one
 * secondary and leaves when it disconnects.
 */
static int
primary(const char *cdev, const char *path, uint64_t offset)
{
	struct vfio_region_info region = {.index = 0};
	struct iommufd iommufd = {0};
	struct vfio_cdev dev = {0};
	char byte;
	void *bar0;
	int listener = -1, sock = -1;
	int err;

	err = iommufd_open(&iommufd);
	if (err) {
		say("primary", "iommufd_open()", strerror(-err));
		return err;
	}

	err = iommufd_ioas_alloc(&iommufd);
	if (err) {
		say("primary", "iommufd_ioas_alloc()", strerror(-err));
		return err;
	}

	err = vfio_cdev_open(cdev, &dev);
	if (err) {
		say("primary", "vfio_cdev_open()", strerror(-err));
		return err;
	}

	err = iommufd_bind(&iommufd, &dev);
	if (err) {
		say("primary", "iommufd_bind()", strerror(-err));
		return err;
	}

	err = iommufd_attach(&iommufd, &dev);
	if (err) {
		say("primary", "iommufd_attach()", strerror(-err));
		return err;
	}
	say("primary", "bind and attach", "ok");

	if (vfio_device_get_region_info(dev.fd, &region)) {
		say("primary", "GET_REGION_INFO(BAR0)", strerror(errno));
		return -errno;
	}

	bar0 = vfio_map_region(dev.fd, region.size, region.offset);
	if (bar0 == MAP_FAILED) {
		say("primary", "mmap(BAR0)", strerror(errno));
		return -errno;
	}
	printf("  [primary]   %-28s 0x%08" PRIx32 " 0x%08" PRIx32 "\n", "host read",
	       mmio_read32(bar0, offset), mmio_read32(bar0, offset + 4));

	listener = listen_at(path);
	if (listener < 0) {
		say("primary", "listen()", strerror(-listener));
		return listener;
	}
	say("primary", "listening", path);

	sock = accept(listener, NULL, NULL);
	if (sock < 0) {
		say("primary", "accept()", strerror(errno));
		close(listener);
		return -errno;
	}

	err = send_fd(sock, dev.fd);
	if (err) {
		say("primary", "send device fd", strerror(-err));
		close(sock);
		close(listener);
		return err;
	}
	say("primary", "send device fd", "ok");

	/* Hold everything open until the secondary is done with it. */
	while (read(sock, &byte, 1) > 0) {
	}
	say("primary", "secondary disconnected", "ok");

	close(sock);
	close(listener);
	unlink(path);

	return 0;
}

int
main(int argc, char *argv[])
{
	const uint64_t offset = 0;
	int err;

	setvbuf(stdout, NULL, _IOLBF, 0);

	if (argc == 4 && !strcmp(argv[1], "primary")) {
		printf("vfio_share_gpu_probe primary: %s\n", argv[2]);
		err = primary(argv[2], argv[3], offset);
	} else if (argc == 3 && !strcmp(argv[1], "standalone")) {
		printf("vfio_share_gpu_probe standalone: %s\n", argv[2]);
		err = standalone(argv[2], offset);
	} else if (argc == 3 && !strcmp(argv[1], "secondary")) {
		int sock = connect_to(argv[2]);

		printf("vfio_share_gpu_probe secondary: %s\n", argv[2]);
		if (sock < 0) {
			say("secondary", "connect()", strerror(-sock));
			return EXIT_FAILURE;
		}

		err = secondary(sock, offset);
		close(sock);
	} else {
		fprintf(stderr, "usage: %s primary <cdev> <socket>\n", argv[0]);
		fprintf(stderr, "       %s secondary <socket>\n", argv[0]);
		fprintf(stderr, "       %s standalone <cdev>\n", argv[0]);
		return 2;
	}

	return err ? EXIT_FAILURE : EXIT_SUCCESS;
}
