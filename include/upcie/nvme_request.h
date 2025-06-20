/**
 * The 'struct nvme_request' introduced here is a software/driver-level abstraction, not construct
 * defined in the NVMe specification. The 'struct nvme_request' serves two purposes:
 *
 * - Management of spec.compliant command-identifiers
 * - Encpsulation auxiliary data associated with each command as it is "inflight"
 */
#define NVME_REQUEST_POOL_LEN 1024
#define NVME_REQUEST_BITMAP_LEN (NVME_REQUEST_POOL_LEN / 64)

struct nvme_request {
	uint16_t cid; ///< The NVMe command identifier
	void *user;   ///< An arbitrary pointer for caller to pass on to completion
};

struct nvme_request_pool {
	struct nvme_request pool[NVME_REQUEST_POOL_LEN];
	uint64_t bitmap[NVME_REQUEST_BITMAP_LEN];
};

static inline void
nvme_request_pool_init(struct nvme_request_pool *pool)
{
	for (size_t i = 0; i < NVME_REQUEST_POOL_LEN; ++i) {
		pool->pool[i].cid = (uint16_t)i;
	}
	for (size_t i = 0; i < NVME_REQUEST_BITMAP_LEN; ++i) {
		pool->bitmap[i] = 0;
	}
}

/**
 * Allocate a request object
 *
 * The intended use is to call this upon command-submission. The returned object will have a 'cid'
 * attribute usable as command-identifier.
 */
static inline struct nvme_request *
nvme_request_alloc(struct nvme_request_pool *p)
{
	for (size_t w = 0; w < NVME_REQUEST_BITMAP_LEN; ++w) {
		if (p->bitmap[w] != UINT64_MAX) {
			uint64_t inv = ~p->bitmap[w];
			uint32_t b = (uint32_t)__builtin_ctzll(inv);
			size_t cid = (w * 64) + b;
			p->bitmap[w] |= (1ULL << b);
			return &p->pool[cid];
		}
	}

	return NULL; // No available entries
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
	size_t w = cid / 64;
	size_t b = cid % 64;
	pool->bitmap[w] &= ~(1ULL << b);
}

/**
 * Retrieve the request object associated with the given 'cid'
 *
 * The intended purpose here is to obtain the request-object associated with a command upon its
 * completion.
 */
static inline struct nvme_request *
nvme_request_get(struct nvme_request_pool *pool, uint16_t cid)
{
	return &pool->pool[cid];
}

/**
 * Check whether the given 'cid' is in use
 */
static inline int
nvme_request_is_cid_in_use(struct nvme_request_pool *pool, uint16_t cid)
{
	size_t w = cid / 64;
	size_t b = cid % 64;

	return (pool->bitmap[w] >> b) & 1;
}
