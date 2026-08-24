// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * Handing a controller to another process, and what passes between them
 * =====================================================================
 *
 * The server of a controller serves clients over a unix socket. What crosses
 * at attach are descriptors, since nothing else moves one between unrelated
 * processes: the vfio device, the iommufd where there is one, and the heap.
 * Everything after that is a fixed-size message, and none of it is on the I/O
 * path.
 *
 * A client submits I/O itself, on queues it was allocated, ringing its own
 * doorbell. What it does not do is touch the admin queue, because there is one
 * of those and its head and tail are held by value; the server submits admin
 * commands on request instead. That costs nothing that matters, since admin is
 * not hot, and it is what lets the runtime record be written once and read
 * without a lock.
 *
 * Admin commands carry payloads, and the payload does not travel: the client
 * names an address the device can already reach, from memory it registered in
 * the shared address space or was allocated, and the server puts that in the
 * command. So an identify lands in the client's buffer without a copy, and
 * without the server having a mapping of it.
 *
 * The server decides which admin commands it is willing to submit. A client
 * asking to identify a namespace is asking about itself; one asking to format
 * or to sanitize is asking on behalf of everybody attached, and that is the
 * server's call rather than the caller's.
 *
 * @file nvme_cplane.h
 * @version 0.7.0
 */

#ifndef __UPCIE_NVME_DELEGATE_H
#define __UPCIE_NVME_DELEGATE_H

/**
 * Bumped when the message layout changes, or when anything it describes does
 */
#define NVME_CPLANE_VERSION 1U

enum nvme_cplane_op {
	NVME_CPLANE_OP_ATTACH = 1,        ///< Client asks for the runtime; descriptors follow
	NVME_CPLANE_OP_ALLOC_IOQPAIR = 2, ///< Client asks for a queue of a given depth
	NVME_CPLANE_OP_FREE_IOQPAIR = 3,  ///< Client hands a queue back
	NVME_CPLANE_OP_ADMIN_CMD = 4,     ///< Client asks for an admin command to be submitted

	/** Anybody asks which controllers the server holds, and what each has */
	NVME_CPLANE_OP_LIST = 8,
};
/**
 * Controllers one reply can name
 *
 * A cap rather than a promise: a server holding more says so in `nheld`, and a
 * caller wanting the rest asks the controllers it did learn about.
 */
#define NVME_CPLANE_LIST_MAX 16

/**
 * Controllers one reply can name
 *
 * A cap rather than a promise: a server holding more says so in `nheld`, and a
 * caller wanting the rest asks the controllers it did learn about.
 */
#define NVME_CPLANE_LIST_MAX 16

/**
 * One message in either direction, request and reply sharing a layout
 *
 * Fixed size, so there is no framing to get wrong, and small enough that a
 * short read means the peer is gone rather than that more is coming.
 */
struct nvme_cplane_msg {
	uint32_t op;      ///< One of enum nvme_cplane_op
	uint32_t version; ///< NVME_CPLANE_VERSION as the sender knows it
	int32_t status;   ///< Replies only: 0, or a negative errno
	uint32_t nfds;    ///< Replies only: descriptors accompanying this message

	union {
		struct {
			uint64_t record_offset; ///< Where the record sits in the heap
			uint64_t heap_nbytes;   ///< What the client should expect to map
		} attach;
		struct {
			struct nvme_ioqpair allocation; ///< Reply: the queue allocated
			uint16_t depth;                 ///< Request: entries wanted
			uint16_t _rsvd[3];
		} queue;
		struct {
			uint32_t qid; ///< The queue being handed back
		} release;
		struct {
			struct nvme_command cmd;    ///< Request: what to submit
			struct nvme_completion cpl; ///< Reply: what came back
		} admin;
	} u;
};

/**
 * Whether an admin command is one a client may ask for
 *
 * The default is deliberately narrow: reading about the controller and its
 * namespaces is a client's business, and anything that changes state for
 * everyone attached is not. An server with a different policy can refuse more,
 * and should not accept more without knowing who it is serving.
 *
 * @param cmd The command a client asked to have submitted
 *
 * @return Non-zero when the command may be submitted on a client's behalf
 */
static inline int
nvme_cplane_admin_permitted(const struct nvme_command *cmd)
{
	switch (cmd->opc) {
	case 0x02: ///< Get Log Page
	case 0x06: ///< Identify
	case 0x0A: ///< Get Features
		return 1;

	default:
		/* Create and delete queue, set features, format, sanitize,
		 * namespace management, firmware download and commit: all of
		 * them change what other clients see. */
		return 0;
	}
}

#endif /* __UPCIE_NVME_DELEGATE_H */
