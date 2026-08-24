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
 * The owner decides which admin commands it is willing to submit. A consumer
 * asking to identify a namespace is asking about itself; one asking to format
 * or to sanitize is asking on behalf of everybody attached, and that is the
 * owner's call rather than the caller's.
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
			struct nvme_command cmd;    ///< Request: what to submit
			struct nvme_completion cpl; ///< Reply: what came back
		} admin;
	} u;
};

/**
 * Whether an admin command is one a consumer may ask for
 *
 * The default is deliberately narrow: reading about the controller and its
 * namespaces is a consumer's business, and anything that changes state for
 * everyone attached is not. An owner with a different policy can refuse more,
 * and should not accept more without knowing who it is serving.
 *
 * @param cmd The command a consumer asked to have submitted
 *
 * @return Non-zero when the command may be submitted on a consumer's behalf
 */
static inline int
nvme_delegate_admin_permitted(const struct nvme_command *cmd)
{
	switch (cmd->opc) {
	case 0x02: ///< Get Log Page
	case 0x06: ///< Identify
	case 0x0A: ///< Get Features
		return 1;

	default:
		/* Create and delete queue, set features, format, sanitize,
		 * namespace management, firmware download and commit: all of
		 * them change what other consumers see. */
		return 0;
	}
}

#endif /* __UPCIE_NVME_DELEGATE_H */
