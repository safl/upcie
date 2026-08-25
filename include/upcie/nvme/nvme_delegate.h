// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * Handing a controller to another process, and what passes between them
 * =====================================================================
 *
 * The owner of a controller serves consumers over a unix socket. What crosses
 * at attach are descriptors, since nothing else moves one between unrelated
 * processes: the vfio device, the iommufd where there is one, and the heap.
 * Everything after that is a fixed-size message, and none of it is on the I/O
 * path.
 *
 * Asking what an owner is holding is on the same channel, since connecting at
 * all is most of the answer: somebody is serving that identifier.
 *
 * A consumer allocates nothing from the shared heap either, for the same
 * reason it does not touch the admin queue: the allocator is the owner's and
 * its free list has no lock. It asks, and is handed an offset. Allocation
 * belongs on the control plane in any case, being done once for memory used
 * many times.
 *
 * A consumer submits I/O itself, on queues it was granted, ringing its own
 * doorbell. What it does not do is touch the admin queue, because there is one
 * of those and its head and tail are held by value; the owner submits admin
 * commands on request instead. That costs nothing that matters, since admin is
 * not hot, and it is what lets the runtime record be written once and read
 * without a lock.
 *
 * Admin commands carry payloads, and the payload does not travel: the consumer
 * names an address the device can already reach, from memory it registered in
 * the shared address space or was granted, and the owner puts that in the
 * command. So an identify lands in the consumer's buffer without a copy, and
 * without the owner having a mapping of it.
 *
 * Which admin commands an owner submits is the owner's business, and the
 * default is to submit what it is asked. The hook is there so that an owner
 * which one day needs to refuse something has somewhere to do it.
 *
 * @file nvme_delegate.h
 */

#ifndef __UPCIE_NVME_DELEGATE_H
#define __UPCIE_NVME_DELEGATE_H

/**
 * Bumped when the message layout changes, or when anything it describes does
 */
#define NVME_DELEGATE_VERSION 1U

enum nvme_delegate_op {
	NVME_DELEGATE_OP_ATTACH = 1,  ///< Consumer asks for the runtime; descriptors follow
	NVME_DELEGATE_OP_GRANT = 2,   ///< Consumer asks for a queue of a given depth
	NVME_DELEGATE_OP_RELEASE = 3, ///< Consumer hands a queue back
	NVME_DELEGATE_OP_ADMIN = 4,   ///< Consumer asks for an admin command to be submitted
	NVME_DELEGATE_OP_ALLOC = 5,   ///< Consumer asks for DMA memory it cannot allocate
	NVME_DELEGATE_OP_FREE = 6,    ///< Consumer hands that memory back
	NVME_DELEGATE_OP_STATUS = 7,  ///< Anybody asks what the owner is holding
};

/**
 * One message in either direction, request and reply sharing a layout
 *
 * Fixed size, so there is no framing to get wrong, and small enough that a
 * short read means the peer is gone rather than that more is coming.
 */
struct nvme_delegate_msg {
	uint32_t op;      ///< One of enum nvme_delegate_op
	uint32_t version; ///< NVME_DELEGATE_VERSION as the sender knows it
	int32_t status;   ///< Replies only: 0, or a negative errno
	uint32_t nfds;    ///< Replies only: descriptors accompanying this message

	union {
		struct {
			uint64_t record_offset; ///< Where the record sits in the heap
			uint64_t heap_nbytes;   ///< What the consumer should expect to map
			uint64_t bar0_nbytes;   ///< Size of the BAR0 mapping to make
		} attach;
		struct {
			struct nvme_qpair_grant grant; ///< Reply: the queue granted
			uint16_t depth;                ///< Request: entries wanted
			uint16_t _rsvd[3];
		} queue;
		struct {
			uint32_t qid; ///< The queue being handed back
		} release;
		struct {
			uint64_t nbytes; ///< Request: how much is wanted
			uint64_t offset; ///< Reply, and the request when freeing
		} mem;
		struct {
			uint32_t nconsumers; ///< How many are attached right now
			uint32_t nqueues;    ///< How many queues they hold between them
			char bdf[16];        ///< The controller being served
		} status;
		struct {
			struct nvme_command cmd;    ///< Request: what to submit
			struct nvme_completion cpl; ///< Reply: what came back
		} admin;
	} u;
};

#define NVME_DELEGATE_FDS_MAX 3 ///< device, iommufd, heap

/**
 * Send one message, optionally with descriptors attached
 *
 * @param sock A connected SOCK_STREAM unix socket
 * @param msg The message to send
 * @param fds Descriptors to attach, or NULL
 * @param nfds How many, at most NVME_DELEGATE_FDS_MAX
 *
 * @return 0 on success, negative errno on error
 */
static inline int
nvme_delegate_msg_send(int sock, const struct nvme_delegate_msg *msg, const int *fds,
		       uint32_t nfds)
{
	char control[CMSG_SPACE(sizeof(int) * NVME_DELEGATE_FDS_MAX)] = {0};
	struct iovec iov = {.iov_base = (void *)msg, .iov_len = sizeof(*msg)};
	struct msghdr hdr = {0};
	ssize_t nbytes;

	if ((sock < 0) || !msg || (nfds > NVME_DELEGATE_FDS_MAX)) {
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

	nbytes = sendmsg(sock, &hdr, MSG_NOSIGNAL);
	if (nbytes < 0) {
		return -errno;
	}
	if (nbytes != (ssize_t)sizeof(*msg)) {
		return -EPROTO; ///< A fixed-size message either fits or the peer is gone
	}

	return 0;
}

/**
 * Receive one message and any descriptors it carries
 *
 * @param sock A connected SOCK_STREAM unix socket
 * @param msg Pre-allocated message to fill
 * @param fds Pre-allocated array of NVME_DELEGATE_FDS_MAX, or NULL
 * @param nfds Set to how many descriptors arrived
 *
 * @return 0 on success, -ENOTCONN when the peer is gone, negative errno on error
 */
static inline int
nvme_delegate_msg_recv(int sock, struct nvme_delegate_msg *msg, int *fds, uint32_t *nfds)
{
	char control[CMSG_SPACE(sizeof(int) * NVME_DELEGATE_FDS_MAX)] = {0};
	struct iovec iov = {.iov_base = msg, .iov_len = sizeof(*msg)};
	struct msghdr hdr = {0};
	struct cmsghdr *cmsg;
	size_t nread = 0;

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

	/* Descriptors arrive with the first byte, so the ancillary data is read
	 * once and only the payload is topped up. */
	while (nread < sizeof(*msg)) {
		ssize_t nbytes;

		iov.iov_base = (char *)msg + nread;
		iov.iov_len = sizeof(*msg) - nread;

		nbytes = recvmsg(sock, &hdr, 0);
		if (nbytes < 0) {
			if (errno == EINTR) {
				continue;
			}
			return -errno;
		}
		if (nbytes == 0) {
			return -ENOTCONN;
		}

		if (!nread && fds && nfds) {
			cmsg = CMSG_FIRSTHDR(&hdr);
			if (cmsg && (cmsg->cmsg_type == SCM_RIGHTS)) {
				*nfds = (uint32_t)((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int));
				memcpy(fds, CMSG_DATA(cmsg), sizeof(int) * (*nfds));
			}
		}

		nread += (size_t)nbytes;
		hdr.msg_control = NULL;
		hdr.msg_controllen = 0;
	}

	return 0;
}

/**
 * Ask a peer for something and wait for its answer
 *
 * @param sock A connected SOCK_STREAM unix socket
 * @param msg The request, replaced by the reply
 * @param fds Pre-allocated array of NVME_DELEGATE_FDS_MAX, or NULL
 * @param nfds Set to how many descriptors arrived
 *
 * @return 0 on success, the peer's status when it refused, negative errno on error
 */
static inline int
nvme_delegate_request(int sock, struct nvme_delegate_msg *msg, int *fds, uint32_t *nfds)
{
	int err;

	msg->version = NVME_DELEGATE_VERSION;
	msg->status = 0;

	err = nvme_delegate_msg_send(sock, msg, NULL, 0);
	if (err) {
		return err;
	}

	err = nvme_delegate_msg_recv(sock, msg, fds, nfds);
	if (err) {
		return err;
	}

	if (msg->version != NVME_DELEGATE_VERSION) {
		UPCIE_DEBUG("FAILED: peer speaks version(%u), this is version(%u)", msg->version,
			    NVME_DELEGATE_VERSION);
		return -EPROTO;
	}

	return msg->status;
}

/**
 * Who is on the other end of a connection
 *
 * An owner deciding whether to serve somebody needs to know who they are, and
 * SO_PEERCRED is the answer the kernel vouches for: the peer cannot claim a
 * uid it does not have. What an owner does with it is policy and belongs to
 * the owner, which is why nothing here decides.
 *
 * @param sock A connected unix socket
 * @param cred Pre-allocated credentials to fill
 *
 * @return 0 on success, negative errno on error
 */
static inline int
nvme_delegate_peer_cred(int sock, struct ucred *cred)
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

/**
 * Whether an admin command is one a consumer may ask for
 *
 * Accepts everything, and exists so that an owner which needs to intervene has
 * one place to do it rather than having to invent one later. A list of
 * permitted opcodes was the first instinct and it would suggest a boundary
 * that is not there: a consumer holds the device fd, so it can reset the
 * controller without asking, and refusing it a Format here would protect
 * nothing while making the common case answer to a list nobody maintains.
 *
 * An owner serving consumers it does not trust has a larger problem than this
 * function.
 *
 * @param cmd The command a consumer asked to have submitted
 *
 * @return Non-zero when the command may be submitted on a consumer's behalf
 */
static inline int
nvme_delegate_admin_permitted(const struct nvme_command *cmd)
{
	(void)cmd;

	return 1;
}

#endif /* __UPCIE_NVME_DELEGATE_H */
