// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * The control plane of a controller served to other processes
 *
 * A server holding a controller does, on request, what a process using it
 * must not do itself: admin commands, creating and deleting queues, and
 * handing out memory the device can reach. A client keeps the data plane: it
 * submits I/O on its own queues and rings its own doorbells. It never touches
 * the admin queue, whose head and tail the server holds by value, nor
 * allocates from the heap, whose free list has no lock. Admin payloads do not
 * travel: the client names memory the device can reach, and the server puts
 * that address in the command.
 *
 * Requests and replies are fixed-size messages on a SOCK_SEQPACKET unix
 * socket, one message per packet, so a message and the descriptors it carries
 * arrive together or not at all. None of it is on the I/O path.
 *
 * @file nvme_cplane.h
 * @version 0.8.0
 */

#ifndef __UPCIE_NVME_CPLANE_H
#define __UPCIE_NVME_CPLANE_H

/**
 * What a message asks for, or answers
 *
 * A client sends NVME_CPLANE_OP_INIT_CONNECTION once per controller. The reply
 * carries two descriptors, the heap and then BAR0. The heap comes back every
 * time, and a client that has mapped it already closes the new one. Queue,
 * buffer and memory requests are refused before it, since heap offsets mean
 * nothing unmapped; NVME_CPLANE_OP_STATUS and NVME_CPLANE_OP_ADMIN_CMD are not.
 */
enum nvme_cplane_op {
	NVME_CPLANE_OP_INIT_CONNECTION = 1, ///< Client asks for the controller named by index
	NVME_CPLANE_OP_ALLOC_IOQPAIR = 2,   ///< Client asks for a queue of a given depth
	NVME_CPLANE_OP_FREE_IOQPAIR = 3,    ///< Client hands a queue back
	NVME_CPLANE_OP_ADMIN_CMD = 4,       ///< Client asks for an admin command to be submitted
	NVME_CPLANE_OP_ALLOC_BUF = 5,       ///< Client asks for DMA memory it cannot allocate
	NVME_CPLANE_OP_FREE_BUF = 6,        ///< Client hands that memory back
	NVME_CPLANE_OP_STATUS = 7,          ///< Anybody asks what the server is holding
};

/**
 * Bytes of operation payload in every message, whatever the operation
 */
#define NVME_CPLANE_PAYLOAD_NBYTES 80

/**
 * NVME_CPLANE_OP_INIT_CONNECTION: what a client needs to use the controller
 */
struct nvme_cplane_init_connection {
	struct nvme_controller_base ctrlr; ///< Reply: what the controller was opened with
	uint64_t heap_nbytes;              ///< Reply: what the client should expect to map
	uint64_t desc_offset;              ///< Reply: the heap's hostmem_shared_desc
	uint64_t bar0_nbytes;              ///< Reply: size of the BAR0 mapping to make
	char bdf[16];                      ///< Reply: the controller, for the client to check
	uint8_t _rsvd[16];
};

UPCIE_STATIC_ASSERT(sizeof(struct nvme_cplane_init_connection) == NVME_CPLANE_PAYLOAD_NBYTES,
		    "nvme_cplane_init_connection is not NVME_CPLANE_PAYLOAD_NBYTES")

/**
 * NVME_CPLANE_OP_ALLOC_IOQPAIR: a queue pair in the heap
 */
struct nvme_cplane_alloc_ioqpair {
	struct nvme_ioqpair allocation; ///< Reply: the queue allocated
	uint16_t depth;                 ///< Request: entries wanted
	uint8_t _rsvd[46];
};

UPCIE_STATIC_ASSERT(sizeof(struct nvme_cplane_alloc_ioqpair) == NVME_CPLANE_PAYLOAD_NBYTES,
		    "nvme_cplane_alloc_ioqpair is not NVME_CPLANE_PAYLOAD_NBYTES")

/**
 * NVME_CPLANE_OP_FREE_IOQPAIR: a queue pair handed back
 */
struct nvme_cplane_free_ioqpair {
	uint32_t qid; ///< Request: the queue being handed back
	uint8_t _rsvd[76];
};

UPCIE_STATIC_ASSERT(sizeof(struct nvme_cplane_free_ioqpair) == NVME_CPLANE_PAYLOAD_NBYTES,
		    "nvme_cplane_free_ioqpair is not NVME_CPLANE_PAYLOAD_NBYTES")

/**
 * NVME_CPLANE_OP_ADMIN_CMD: an admin command submitted on the client's behalf
 */
struct nvme_cplane_admin_cmd {
	struct nvme_command cmd;    ///< Request: what to submit
	struct nvme_completion cpl; ///< Reply: what came back
};

UPCIE_STATIC_ASSERT(sizeof(struct nvme_cplane_admin_cmd) == NVME_CPLANE_PAYLOAD_NBYTES,
		    "nvme_cplane_admin_cmd is not NVME_CPLANE_PAYLOAD_NBYTES")

/**
 * NVME_CPLANE_OP_ALLOC_BUF and NVME_CPLANE_OP_FREE_BUF: heap memory
 */
struct nvme_cplane_buf {
	uint64_t nbytes; ///< Request: how much is wanted
	uint64_t offset; ///< Reply, and the request when freeing
	uint8_t _rsvd[64];
};

UPCIE_STATIC_ASSERT(sizeof(struct nvme_cplane_buf) == NVME_CPLANE_PAYLOAD_NBYTES,
		    "nvme_cplane_buf is not NVME_CPLANE_PAYLOAD_NBYTES")

/**
 * NVME_CPLANE_OP_STATUS: what the server is holding
 */
struct nvme_cplane_status {
	uint32_t ndevs;          ///< Reply: controllers this server holds
	uint32_t nclients;       ///< Reply: connections over all controllers
	uint32_t nclients_ctrlr; ///< Reply: of those, given this controller's fds
	uint32_t nqueues;        ///< Reply: queues they hold on this controller
	uint32_t nsq_total;      ///< Reply: Number of Queues feature, 0 if unanswered
	uint32_t ncq_total;      ///< Reply: Number of Queues feature, 0 if unanswered
	char bdf[16];            ///< Reply: the controller being served
	uint8_t _rsvd[40];
};

UPCIE_STATIC_ASSERT(sizeof(struct nvme_cplane_status) == NVME_CPLANE_PAYLOAD_NBYTES,
		    "nvme_cplane_status is not NVME_CPLANE_PAYLOAD_NBYTES")

/**
 * One message in either direction, request and reply sharing a layout
 *
 * One socket serves every controller, and a client learns the range of index
 * from NVME_CPLANE_OP_STATUS at index zero.
 */
struct nvme_cplane_msg {
	uint32_t op;      ///< One of enum nvme_cplane_op
	uint32_t version; ///< NVME_CPLANE_VERSION as the sender knows it
	int32_t result;   ///< Replies only: 0, or a negative errno
	uint32_t nfds;    ///< Replies only: descriptors accompanying this message
	uint32_t index;   ///< Which of the server's controllers, from zero
	uint32_t _rsvd;

	union {
		struct nvme_cplane_init_connection init_connection;
		struct nvme_cplane_alloc_ioqpair alloc_ioqpair;
		struct nvme_cplane_free_ioqpair free_ioqpair;
		struct nvme_cplane_admin_cmd admin_cmd;
		struct nvme_cplane_buf buf;
		struct nvme_cplane_status status;
	} u; ///< Per-operation payload, named by op
};

UPCIE_STATIC_ASSERT(sizeof(struct nvme_cplane_msg) == 24 + NVME_CPLANE_PAYLOAD_NBYTES,
		    "nvme_cplane_msg changed size; bump NVME_CPLANE_VERSION")

#define NVME_CPLANE_FDS_MAX 3 ///< Most descriptors one message carries

/**
 * Send one message, optionally with descriptors attached
 *
 * @param sock A connected SOCK_SEQPACKET unix socket
 * @param msg The message to send
 * @param fds Descriptors to attach, or NULL
 * @param nfds How many, at most NVME_CPLANE_FDS_MAX
 * @param flags Passed to sendmsg(2); MSG_DONTWAIT to avoid waiting
 *
 * @return 0 on success, -EAGAIN when MSG_DONTWAIT was given and the peer is
 *         not reading, -EPIPE when the peer is gone, negative errno on error
 */
static inline int
nvme_cplane_msg_send_flags(int sock, const struct nvme_cplane_msg *msg, const int *fds,
			   uint32_t nfds, int flags)
{
	char control[CMSG_SPACE(sizeof(int) * NVME_CPLANE_FDS_MAX)] = {0};
	struct iovec iov = {.iov_base = (void *)msg, .iov_len = sizeof(*msg)};
	struct msghdr hdr = {0};
	ssize_t nbytes;

	if ((sock < 0) || !msg || (nfds > NVME_CPLANE_FDS_MAX) || (nfds && !fds)) {
		return -EINVAL;
	}

	hdr.msg_iov = &iov;
	hdr.msg_iovlen = 1;

	if (nfds) {
		struct cmsghdr *cmsg;

		hdr.msg_control = control;
		hdr.msg_controllen = CMSG_SPACE(sizeof(int) * nfds);

		cmsg = CMSG_FIRSTHDR(&hdr);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = CMSG_LEN(sizeof(int) * nfds);
		memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * nfds);
	}

	do {
		nbytes = sendmsg(sock, &hdr, MSG_NOSIGNAL | flags);
	} while ((nbytes < 0) && (errno == EINTR));
	if (nbytes < 0) {
		return -errno;
	}
	if (nbytes != (ssize_t)sizeof(*msg)) {
		return -EPROTO;
	}

	return 0;
}

/**
 * Send one message, waiting for room where there is none
 *
 * @param sock A connected SOCK_SEQPACKET unix socket
 * @param msg The message to send
 * @param fds Descriptors to attach, or NULL
 * @param nfds How many, at most NVME_CPLANE_FDS_MAX
 *
 * @return 0 on success, -EPIPE when the peer is gone, negative errno on error
 */
static inline int
nvme_cplane_msg_send(int sock, const struct nvme_cplane_msg *msg, const int *fds, uint32_t nfds)
{
	return nvme_cplane_msg_send_flags(sock, msg, fds, nfds, 0);
}

/**
 * Send one message, or say that the peer is not reading
 *
 * For a single-threaded server, where a client that stops reading must not
 * stall every other connection. The socket buffer holds many replies, so
 * -EAGAIN means the client should be dropped.
 *
 * @param sock A connected SOCK_SEQPACKET unix socket
 * @param msg The message to send
 * @param fds Descriptors to attach, or NULL
 * @param nfds How many, at most NVME_CPLANE_FDS_MAX
 *
 * @return 0 on success, -EAGAIN when the peer is not reading, -EPIPE when the
 *         peer is gone, negative errno on error
 */
static inline int
nvme_cplane_msg_trysend(int sock, const struct nvme_cplane_msg *msg, const int *fds, uint32_t nfds)
{
	return nvme_cplane_msg_send_flags(sock, msg, fds, nfds, MSG_DONTWAIT);
}

/**
 * Close descriptors a receive installed, and forget them
 *
 * @param fds Descriptors as filled by a receive, or NULL
 * @param nfds How many; set to zero
 */
static inline void
nvme_cplane_fds_close(int *fds, uint32_t *nfds)
{
	if (!fds || !nfds) {
		return;
	}
	for (uint32_t i = 0; i < *nfds; ++i) {
		close(fds[i]);
	}
	*nfds = 0;
}

/**
 * Receive one message and the descriptors that came with it
 *
 * Received descriptors are installed whether or not they were asked for, so
 * with fds or nfds NULL they are closed here rather than leaked. A packet of
 * another size than a message, more than NVME_CPLANE_FDS_MAX descriptors, or
 * MSG_CTRUNC, which leaves a short list that looks complete, is refused with
 * -EPROTO after closing everything that arrived.
 *
 * @param sock A connected SOCK_SEQPACKET unix socket
 * @param msg Pre-allocated message to fill
 * @param fds Pre-allocated array of NVME_CPLANE_FDS_MAX, or NULL to refuse any
 * @param nfds Set to how many descriptors arrived; NULL refuses any
 * @param flags Passed to recvmsg(2); MSG_DONTWAIT to avoid waiting
 *
 * @return 0 on success, -ENOTCONN when the peer is gone, whether it closed or
 *         went away with a reply unread, -EAGAIN when nothing has arrived and
 *         the socket would block or its receive timeout expired, negative errno
 *         on error; on error nfds is zero
 */
static inline int
nvme_cplane_msg_recv_flags(int sock, struct nvme_cplane_msg *msg, int *fds, uint32_t *nfds,
			   int flags)
{
	char control[CMSG_SPACE(sizeof(int) * NVME_CPLANE_FDS_MAX)] = {0};
	struct iovec iov = {.iov_base = (void *)msg, .iov_len = sizeof(*msg)};
	struct msghdr hdr = {0};
	struct cmsghdr *cmsg;
	int got[NVME_CPLANE_FDS_MAX];
	uint32_t ngot = 0;
	int bad, excess = 0;
	ssize_t nbytes;

	if ((sock < 0) || !msg) {
		return -EINVAL;
	}
	if (nfds) {
		*nfds = 0;
	}

	hdr.msg_iov = &iov;
	hdr.msg_iovlen = 1;
	hdr.msg_control = control;
	hdr.msg_controllen = sizeof(control);

	do {
		nbytes = recvmsg(sock, &hdr, MSG_CMSG_CLOEXEC | flags);
	} while ((nbytes < 0) && (errno == EINTR));
	if (nbytes < 0) {
		return (errno == ECONNRESET) ? -ENOTCONN : -errno;
	}
	if (!nbytes && !CMSG_FIRSTHDR(&hdr) && !(hdr.msg_flags & MSG_CTRUNC)) {
		return -ENOTCONN;
	}

	for (cmsg = CMSG_FIRSTHDR(&hdr); cmsg; cmsg = CMSG_NXTHDR(&hdr, cmsg)) {
		size_t n;

		if ((cmsg->cmsg_level != SOL_SOCKET) || (cmsg->cmsg_type != SCM_RIGHTS)) {
			continue;
		}
		n = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
		for (size_t i = 0; i < n; ++i) {
			int fd;

			memcpy(&fd, CMSG_DATA(cmsg) + i * sizeof(int), sizeof(fd));
			if (ngot < NVME_CPLANE_FDS_MAX) {
				got[ngot++] = fd;
			} else {
				close(fd);
				excess = 1;
			}
		}
	}

	bad = excess || (hdr.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) ||
	      (nbytes != (ssize_t)sizeof(*msg));
	if (bad || !fds || !nfds) {
		for (uint32_t i = 0; i < ngot; ++i) {
			close(got[i]);
		}
		if (bad) {
			UPCIE_DEBUG("FAILED: not a message; nbytes(%zd) flags(0x%x) excess(%d)",
				    nbytes, hdr.msg_flags, excess);
			return -EPROTO;
		}
		return 0;
	}

	*nfds = ngot;
	memcpy(fds, got, sizeof(int) * ngot);

	return 0;
}

/**
 * Receive one message, waiting for it
 *
 * @param sock A connected SOCK_SEQPACKET unix socket
 * @param msg Pre-allocated message to fill
 * @param fds Pre-allocated array of NVME_CPLANE_FDS_MAX, or NULL to refuse any
 * @param nfds Set to how many descriptors arrived
 *
 * @return As nvme_cplane_msg_recv_flags()
 */
static inline int
nvme_cplane_msg_recv(int sock, struct nvme_cplane_msg *msg, int *fds, uint32_t *nfds)
{
	return nvme_cplane_msg_recv_flags(sock, msg, fds, nfds, 0);
}

/**
 * Receive one message when one has arrived, without waiting
 *
 * For a server multiplexing connections on one thread, calling it when the
 * socket is readable.
 *
 * @param sock A connected SOCK_SEQPACKET unix socket
 * @param msg Pre-allocated message to fill
 * @param fds Pre-allocated array of NVME_CPLANE_FDS_MAX, or NULL to refuse any
 * @param nfds Set to how many descriptors arrived
 *
 * @return As nvme_cplane_msg_recv_flags(); -EAGAIN when nothing has arrived
 */
static inline int
nvme_cplane_msg_tryrecv(int sock, struct nvme_cplane_msg *msg, int *fds, uint32_t *nfds)
{
	return nvme_cplane_msg_recv_flags(sock, msg, fds, nfds, MSG_DONTWAIT);
}

/**
 * Ask a peer for something and wait for its answer
 *
 * @param sock A connected SOCK_SEQPACKET unix socket
 * @param msg The request, replaced by the reply
 * @param fds Pre-allocated array of NVME_CPLANE_FDS_MAX, or NULL
 * @param nfds Set to how many descriptors arrived
 *
 * @return 0 on success, the peer's status when it refused, negative errno on error;
 *         unless it is 0, any descriptors that arrived are closed and nfds is zero.
 *         After -EAGAIN from a receive timeout the reply may still arrive, and
 *         no message says which request it answers, so drop the connection
 */
static inline int
nvme_cplane_request(int sock, struct nvme_cplane_msg *msg, int *fds, uint32_t *nfds)
{
	int err;

	msg->version = NVME_CPLANE_VERSION;
	msg->result = 0;

	err = nvme_cplane_msg_send(sock, msg, NULL, 0);
	if (err) {
		return err;
	}

	err = nvme_cplane_msg_recv(sock, msg, fds, nfds);
	if (err) {
		return err;
	}

	if (msg->version != NVME_CPLANE_VERSION) {
		UPCIE_DEBUG("FAILED: peer speaks version(%u), this is version(%u)", msg->version,
			    NVME_CPLANE_VERSION);
		nvme_cplane_fds_close(fds, nfds);
		return -EPROTO;
	}
	if (msg->result) {
		nvme_cplane_fds_close(fds, nfds);
	}

	return msg->result;
}

/**
 * Who is on the other end of a connection
 *
 * SO_PEERCRED is vouched for by the kernel, so the peer cannot claim a uid it
 * does not have. What to do with it is the server's policy.
 *
 * @param sock A connected unix socket
 * @param cred Pre-allocated credentials to fill
 *
 * @return 0 on success, negative errno on error
 */
static inline int
nvme_cplane_peer_cred(int sock, struct ucred *cred)
{
	socklen_t nbytes = sizeof(*cred);

	if ((sock < 0) || !cred) {
		return -EINVAL;
	}

	if (getsockopt(sock, SOL_SOCKET, SO_PEERCRED, cred, &nbytes)) {
		return -errno;
	}

	return 0;
}

#endif /* __UPCIE_NVME_CPLANE_H */
