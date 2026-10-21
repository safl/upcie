// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * Exercise the control-plane messaging over a socketpair
 * ======================================================
 *
 * Sending, receiving and descriptor passing need a unix socket and nothing
 * else, so no device is needed. A leaked descriptor is invisible to the
 * caller, so the tests count this process's open descriptors instead.
 */
#define _GNU_SOURCE
#define _UPCIE_WITH_NVME
#include <upcie/upcie.h>
#include <sys/time.h>

static int
nfds_open(void)
{
	struct dirent *ent;
	DIR *dir;
	int count = 0;

	dir = opendir("/proc/self/fd");
	assert(dir);
	while ((ent = readdir(dir))) {
		if (ent->d_name[0] != '.') {
			count++;
		}
	}
	closedir(dir);

	return count;
}

/**
 * Send a message with n descriptors attached, bypassing the cap that
 * nvme_cplane_msg_send() enforces, as a misbehaving peer would
 */
static void
send_with_n_fds(int sock, int n)
{
	char control[CMSG_SPACE(sizeof(int) * 8)] = {0};
	struct nvme_cplane_msg msg = {.version = NVME_CPLANE_VERSION};
	struct iovec iov = {.iov_base = &msg, .iov_len = sizeof(msg)};
	struct msghdr hdr = {0};
	struct cmsghdr *cmsg;
	int fds[8];

	assert(n <= 8);
	for (int i = 0; i < n; ++i) {
		fds[i] = open("/dev/null", O_RDONLY);
		assert(fds[i] >= 0);
	}

	hdr.msg_iov = &iov;
	hdr.msg_iovlen = 1;
	hdr.msg_control = control;
	hdr.msg_controllen = CMSG_SPACE(sizeof(int) * n);
	cmsg = CMSG_FIRSTHDR(&hdr);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int) * n);
	memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * n);

	assert(sendmsg(sock, &hdr, 0) == (ssize_t)sizeof(msg));

	for (int i = 0; i < n; ++i) {
		close(fds[i]);
	}
}

static int
test_too_many_fds_are_refused_and_closed(void)
{
	for (int n = NVME_CPLANE_FDS_MAX + 1; n <= NVME_CPLANE_FDS_MAX + 2; ++n) {
		struct nvme_cplane_msg msg = {0};
		int fds[NVME_CPLANE_FDS_MAX];
		uint32_t nfds = 0;
		int sv[2], before, err;

		assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv));
		before = nfds_open();

		send_with_n_fds(sv[0], n);
		err = nvme_cplane_msg_recv(sv[1], &msg, fds, &nfds);
		if (err != -EPROTO || nfds) {
			printf("FAILED: %d fds gave err(%d) nfds(%u), expected EPROTO\n", n, err,
			       nfds);
			return 1;
		}
		if (nfds_open() != before) {
			printf("FAILED: %d fds left %d open\n", n, nfds_open() - before);
			return 1;
		}

		close(sv[0]);
		close(sv[1]);
	}

	printf("PASSED: more than NVME_CPLANE_FDS_MAX is refused and closed\n");
	return 0;
}

static int
test_unwanted_fds_are_closed(void)
{
	struct nvme_cplane_msg msg = {.version = NVME_CPLANE_VERSION};
	int sv[2], fds[2], before, err;

	assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv));
	before = nfds_open();

	fds[0] = open("/dev/null", O_RDONLY);
	fds[1] = open("/dev/null", O_RDONLY);
	assert(!nvme_cplane_msg_send(sv[0], &msg, fds, 2));
	close(fds[0]);
	close(fds[1]);

	err = nvme_cplane_msg_recv(sv[1], &msg, NULL, NULL);
	if (err) {
		printf("FAILED: receive refusing fds gave err(%d)\n", err);
		return 1;
	}
	if (nfds_open() != before) {
		printf("FAILED: refused fds left %d open\n", nfds_open() - before);
		return 1;
	}

	close(sv[0]);
	close(sv[1]);

	printf("PASSED: descriptors nobody asked for are closed\n");
	return 0;
}

static int
test_fds_arrive(void)
{
	struct nvme_cplane_msg msg = {.version = NVME_CPLANE_VERSION};
	int sent[NVME_CPLANE_FDS_MAX], got[NVME_CPLANE_FDS_MAX];
	uint32_t ngot = 0;
	int sv[2], before, err;

	assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv));
	before = nfds_open();

	for (int i = 0; i < NVME_CPLANE_FDS_MAX; ++i) {
		sent[i] = open("/dev/null", O_RDONLY);
	}
	assert(!nvme_cplane_msg_send(sv[0], &msg, sent, NVME_CPLANE_FDS_MAX));
	for (int i = 0; i < NVME_CPLANE_FDS_MAX; ++i) {
		close(sent[i]);
	}

	err = nvme_cplane_msg_recv(sv[1], &msg, got, &ngot);
	if (err || ngot != NVME_CPLANE_FDS_MAX) {
		printf("FAILED: err(%d) ngot(%u), expected %d fds\n", err, ngot,
		       NVME_CPLANE_FDS_MAX);
		return 1;
	}
	nvme_cplane_fds_close(got, &ngot);
	if (nfds_open() != before) {
		printf("FAILED: %d left open after closing what arrived\n", nfds_open() - before);
		return 1;
	}

	close(sv[0]);
	close(sv[1]);

	printf("PASSED: NVME_CPLANE_FDS_MAX descriptors arrive\n");
	return 0;
}

static int
test_send_refuses_null_fds(void)
{
	struct nvme_cplane_msg msg = {0};
	int sv[2], err;

	assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv));

	err = nvme_cplane_msg_send(sv[0], &msg, NULL, 2);
	if (err != -EINVAL) {
		printf("FAILED: NULL fds with nfds(2) gave err(%d)\n", err);
		return 1;
	}

	close(sv[0]);
	close(sv[1]);

	printf("PASSED: send refuses NULL fds with a count\n");
	return 0;
}

static int
test_recv_returns_on_timeout(void)
{
	struct timeval tv = {.tv_usec = 10000};
	struct nvme_cplane_msg msg = {0};
	int sv[2], err;

	assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv));
	assert(!setsockopt(sv[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)));

	err = nvme_cplane_msg_recv(sv[1], &msg, NULL, NULL);
	if (err != -EAGAIN) {
		printf("FAILED: an idle socket with a timeout gave err(%d)\n", err);
		return 1;
	}

	err = nvme_cplane_msg_tryrecv(sv[1], &msg, NULL, NULL);
	if (err != -EAGAIN) {
		printf("FAILED: tryrecv on an idle socket gave err(%d)\n", err);
		return 1;
	}

	close(sv[0]);
	close(sv[1]);

	printf("PASSED: receive returns when nothing arrives\n");
	return 0;
}

static int
test_wrong_sized_packets_are_refused(void)
{
	char packet[2 * sizeof(struct nvme_cplane_msg)] = {0};
	size_t sizes[] = {sizeof(struct nvme_cplane_msg) / 2, sizeof(packet)};

	for (size_t i = 0; i < sizeof(sizes) / sizeof(*sizes); ++i) {
		struct nvme_cplane_msg msg = {0};
		int sv[2], err;

		assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv));

		assert(write(sv[0], packet, sizes[i]) == (ssize_t)sizes[i]);
		err = nvme_cplane_msg_recv(sv[1], &msg, NULL, NULL);
		if (err != -EPROTO) {
			printf("FAILED: a packet of %zu bytes gave err(%d)\n", sizes[i], err);
			return 1;
		}

		close(sv[0]);
		close(sv[1]);
	}

	printf("PASSED: a packet that is not one message is refused\n");
	return 0;
}

static int
test_recv_sees_the_peer_go(void)
{
	struct nvme_cplane_msg msg = {0};
	int sv[2], err;

	assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv));
	close(sv[0]);

	err = nvme_cplane_msg_recv(sv[1], &msg, NULL, NULL);
	if (err != -ENOTCONN) {
		printf("FAILED: a closed peer gave err(%d)\n", err);
		return 1;
	}

	close(sv[1]);

	assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv));
	msg.version = NVME_CPLANE_VERSION;
	assert(!nvme_cplane_msg_send(sv[1], &msg, NULL, 0));
	close(sv[0]);

	err = nvme_cplane_msg_recv(sv[1], &msg, NULL, NULL);
	if (err != -ENOTCONN) {
		printf("FAILED: a peer gone with a message unread gave err(%d)\n", err);
		return 1;
	}

	close(sv[1]);

	printf("PASSED: receive sees the peer go\n");
	return 0;
}

int
main(void)
{
	int failed = 0;

	failed += test_too_many_fds_are_refused_and_closed();
	failed += test_unwanted_fds_are_closed();
	failed += test_fds_arrive();
	failed += test_send_refuses_null_fds();
	failed += test_recv_returns_on_timeout();
	failed += test_wrong_sized_packets_are_refused();
	failed += test_recv_sees_the_peer_go();

	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
