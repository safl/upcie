// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * NVMe Request Abstraction
 * ========================
 *
 * This header defines a minimal software abstraction for managing NVMe command identifiers (CIDs)
 * in user space. The abstraction uses a fixed-size pool of `struct nvme_request`, each assigned a
 * CID, along with a freelist-based allocator for constant-time allocation and release.
 *
 * This is not part of the NVMe specification, but is useful for tracking user-submitted commands
 * while they are in flight and associating user-defined metadata with each command.
 *
 * Caveat
 * ------
 *
 * assert() is used here. thus instead of a segfault, you will get a nice message like::
 *
 *   nvme_request_get: Assertion `cid < NVME_REQUEST_POOL_LEN' failed.
 *
 * Of course, this comes at a cost, so, make sure e.g. meson disables assert on release builds.
 *
 * The stack implementation has an upper-bound of NVME_REQUEST_POOL_LEN elements.
 *
 * @file nvme_request.h
 * @version 0.3.0
 */

#define NVME_REQUEST_POOL_LEN 1024

struct nvme_request {
	uint16_t cid; ///< The NVMe command identifier
	void *user;   ///< An arbitrary pointer for caller to pass on to completion
};

struct nvme_request_pool {
	struct nvme_request reqs[NVME_REQUEST_POOL_LEN];
	uint16_t stack[NVME_REQUEST_POOL_LEN];
	size_t top;
};

static inline void
nvme_request_pool_init(struct nvme_request_pool *pool)
{
	pool->top = NVME_REQUEST_POOL_LEN;
	for (uint16_t i = 0; i < NVME_REQUEST_POOL_LEN; ++i) {
		pool->reqs[i].cid = i;
		pool->stack[NVME_REQUEST_POOL_LEN - 1 - i] = i;
	}
}

/**
 * Allocates a request object from the pool.
 *
 * The returned request has a valid CID and may be used for command submission.
 *
 * @param pool The request pool to allocate from.
 * @return On success, a pointer to a request is returned. On error, NULL is returned and errno set
 *         to indicate the error.
 */
static inline struct nvme_request *
nvme_request_alloc(struct nvme_request_pool *pool)
{
	uint16_t cid;

	assert(pool->top > 0);

	if (pool->top == 0) {
		errno = ENOMEM;
		return NULL;
	}

	cid = pool->stack[--pool->top];

	return &pool->reqs[cid];
}

/**
 * Free a request previously allocated with nvme_request_alloc().
 *
 * This marks the `cid` (command-identifier) as available for reuse.
 *
 * The `cid` must no longer be referenced in any submission or completion queue -- that is, the
 * associated command must be fully completed, and any processing of the completion must be done.
 * Only then is it safe to free the request.
 *
 * If you're using submit-on-completion (i.e., reusing the request immediately), there is no need
 * to call this function -- the `cid` is implicitly reused.
 *
 * @param pool The request pool the `cid` came from.
 * @param cid  The command identifier to mark as available again.
 */
static inline void
nvme_request_free(struct nvme_request_pool *pool, uint16_t cid)
{
	assert(pool->top < NVME_REQUEST_POOL_LEN);
	pool->stack[pool->top++] = cid;
}

/**
 * Retrieve the request object associated with the given 'cid'
 *
 * The intended purpose here is to obtain the request-object associated with a command upon its
 * completion.
 *
 * @param pool The request pool the CID belongs to.
 * @param cid The command identifier.
 * @return Pointer to the corresponding request object.
 */
static inline struct nvme_request *
nvme_request_get(struct nvme_request_pool *pool, uint16_t cid)
{
	assert(cid < NVME_REQUEST_POOL_LEN);
	return &pool->reqs[cid];
}