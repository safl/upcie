// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * Registry of externally-provided DMA-able regions
 * ================================================
 *
 * Where a dmamem describes one contiguous range, a registry describes an
 * arbitrary number of them, so a caller can hand over memory it already owns
 * and have the device DMA into it. That is the difference between "use the
 * buffers the library allocated" and "use the buffers the application
 * allocated", and the latter is what a framework holding its own GPU tensors
 * or hugepage arenas needs.
 *
 * The registry is flavour-agnostic. What differs between CUDA, HIP and
 * hugepages is only how a chunk's bus address is discovered, which is the
 * `populate` callback; the structure, the lookup and the lifetime rules are
 * common. Anything whose backing is pinned and contiguous at `granularity`
 * can be registered: device allocations, hugepages, BAR ranges, dma-buf
 * exports.
 *
 * Chunks are indexed directly, so lookup is one load regardless of how many
 * regions are registered:
 *
 *     chunk_idx = vaddr >> gran_shift
 *     phys      = lut_phys[chunk_idx] + (vaddr & gran_mask)
 *
 * Two parallel arrays cover the chunk_idx range, both MAP_NORESERVE so the
 * kernel demand-pages them and the resident cost tracks live chunks rather
 * than virtual capacity: `lut_phys` on the hot path, `lut_meta` for the cold
 * path. Chunks are refcounted, so overlapping registrations amortise to one
 * population, and resolve to identical addresses for any VA they share.
 *
 * Sizing
 * ------
 *
 * The reservation is `(1 << va_bits) / granularity` slots, so it scales
 * inversely with granularity: at 48-bit VA that is 1 GiB of lut_phys for a
 * 2 MiB granularity but 512 GiB for a 4 KiB one. Callers with a small
 * granularity should pass a smaller `va_bits` bounded by the address range
 * they actually use.
 *
 * Adoption
 * --------
 *
 * A caller that already knows the physical addresses, such as a heap that
 * enumerated them at init, registers with `dmamem_registry_adopt()` rather
 * than paying to rediscover them. Adopted chunks are marked borrowed and are
 * never released by the registry.
 *
 * @file dmamem_registry.h
 * @version 0.6.0
 */

/**
 * Default width of the virtual address space, in bits, used to size the LUTs.
 */
#define DMAMEM_REGISTRY_VA_BITS 48

/**
 * Discover the bus address of one `granularity`-sized, `granularity`-aligned
 * chunk.
 *
 * Called only for chunks not already live. On success the chunk's base
 * address is returned via `phys_base_out`, and any attachment that must be
 * undone later via `attach_out`; a flavour with nothing to release leaves it
 * zeroed. On failure both outputs are left untouched.
 *
 * @return 0 on success, negative errno on failure.
 */
typedef int (*dmamem_registry_populate_fn)(void *ctx, uint64_t chunk_va, size_t granularity,
					   uint64_t *phys_base_out, struct dmabuf *attach_out);

/**
 * Undo what populate did for one chunk. May be NULL when nothing is owned.
 */
typedef void (*dmamem_registry_release_fn)(void *ctx, struct dmabuf *attach);

/**
 * Per-chunk state, consulted off the hot path.
 *
 * `rc` counts registrations whose floored range intersects this chunk; the
 * chunk is populated on the transition to one and released on the transition
 * back to zero. The chunk's address lives in `lut_phys`, not here, so the hot
 * path needs a single load.
 */
struct dmamem_registry_chunk_meta {
	uint32_t rc;          ///< Refcount of overlapping registrations
	uint32_t borrowed;    ///< 1: address was adopted, there is nothing to release
	struct dmabuf attach; ///< Attachment owned by the registry (valid when rc > 0 && !borrowed)
};

/**
 * One registration, so removal can find the chunks to drop.
 */
struct dmamem_registry_registration {
	uint64_t vaddr;                             ///< Start of the registered range
	size_t size;                                ///< Length of the registered range in bytes
	struct dmamem_registry_registration *next;  ///< List linkage owned by the registry
};

struct dmamem_registry {
	int gran_shift;      ///< log2(granularity)
	uint64_t gran_mask;  ///< granularity - 1, for the intra-chunk offset
	size_t lut_capacity; ///< Number of slots in each LUT
	uint64_t *lut_phys;  ///< chunk_idx -> chunk base address; mmap-backed
	struct dmamem_registry_chunk_meta *lut_meta; ///< chunk_idx -> state; mmap-backed
	struct dmamem_registry_registration *list;   ///< Owned list of registrations
	dmamem_registry_populate_fn populate;        ///< Discovers a chunk's address
	dmamem_registry_release_fn release;          ///< Undoes populate; may be NULL
	void *ctx;                                   ///< Passed to populate/release; not owned
};

/**
 * Initialize a registry.
 *
 * Reserves the two demand-paged LUTs; no physical memory is committed until
 * chunks are registered. `granularity` must be a power of two, and every
 * region registered must be contiguous in bus-address terms across it.
 *
 * @param granularity Chunk size in bytes; a power of two
 * @param va_bits     Width of the address range to cover; 0 selects the default
 * @param populate    Discovers a chunk's address; required
 * @param release     Undoes populate; NULL when nothing is owned
 * @param ctx         Opaque flavour context handed to the callbacks
 *
 * @return 0 on success, negative errno on failure.
 */
static inline int
dmamem_registry_init(struct dmamem_registry *registry, size_t granularity, int va_bits,
		     dmamem_registry_populate_fn populate, dmamem_registry_release_fn release,
		     void *ctx)
{
	size_t phys_bytes, meta_bytes;
	int gran_shift = 0;

	if (!registry || !granularity || (granularity & (granularity - 1)) || !populate) {
		return -EINVAL;
	}

	while (((size_t)1 << gran_shift) < granularity) {
		++gran_shift;
	}

	if (!va_bits) {
		va_bits = DMAMEM_REGISTRY_VA_BITS;
	}
	/* lut_capacity is 1 << (va_bits - gran_shift), so the difference has to
	 * stay a shift a size_t can take. */
	if ((va_bits <= gran_shift) || (va_bits > 64) || ((va_bits - gran_shift) >= 64)) {
		return -EINVAL;
	}

	memset(registry, 0, sizeof(*registry));
	registry->gran_shift = gran_shift;
	registry->gran_mask = (uint64_t)granularity - 1;
	registry->lut_capacity = (size_t)1 << (va_bits - gran_shift);
	registry->populate = populate;
	registry->release = release;
	registry->ctx = ctx;

	phys_bytes = registry->lut_capacity * sizeof(*registry->lut_phys);
	registry->lut_phys = mmap(NULL, phys_bytes, PROT_READ | PROT_WRITE,
				  MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (registry->lut_phys == MAP_FAILED) {
		UPCIE_DEBUG("FAILED: mmap(lut_phys, %zu); errno: %d", phys_bytes, errno);
		registry->lut_phys = NULL;
		return -ENOMEM;
	}

	meta_bytes = registry->lut_capacity * sizeof(*registry->lut_meta);
	registry->lut_meta = mmap(NULL, meta_bytes, PROT_READ | PROT_WRITE,
				  MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (registry->lut_meta == MAP_FAILED) {
		UPCIE_DEBUG("FAILED: mmap(lut_meta, %zu); errno: %d", meta_bytes, errno);
		munmap(registry->lut_phys, phys_bytes);
		registry->lut_phys = NULL;
		registry->lut_meta = NULL;
		return -ENOMEM;
	}

	return 0;
}

/**
 * Drop a reference on chunks [chunk_first, chunk_first + chunk_cnt),
 * releasing each chunk whose refcount reaches zero.
 *
 * Chunks already at zero are skipped, so this is safe for partial unwind
 * where only some chunks in the range were bumped.
 */
static inline void
dmamem_registry_chunk_deref(struct dmamem_registry *registry, size_t chunk_first, size_t chunk_cnt)
{
	for (size_t k = 0; k < chunk_cnt; ++k) {
		const size_t idx = chunk_first + k;
		struct dmamem_registry_chunk_meta *cm = &registry->lut_meta[idx];

		if (cm->rc == 0) {
			continue;
		}
		cm->rc--;
		if (cm->rc) {
			continue;
		}

		if (!cm->borrowed && registry->release) {
			registry->release(registry->ctx, &cm->attach);
		}
		memset(cm, 0, sizeof(*cm));
		registry->lut_phys[idx] = 0;
	}
}

/**
 * Drop every registration, releasing the chunks they held. The LUT
 * reservations stay, so the registry remains usable.
 */
static inline void
dmamem_registry_clear(struct dmamem_registry *registry)
{
	struct dmamem_registry_registration *next;

	if (!registry) {
		return;
	}

	const uint64_t mask = registry->gran_mask;
	const int gran_shift = registry->gran_shift;

	for (struct dmamem_registry_registration *m = registry->list; m; m = next) {
		const size_t chunk_first = (size_t)(m->vaddr >> gran_shift);
		const size_t chunk_cnt =
			(size_t)(((m->vaddr & mask) + m->size + mask) >> gran_shift);

		dmamem_registry_chunk_deref(registry, chunk_first, chunk_cnt);

		next = m->next;
		free(m);
	}
	registry->list = NULL;
}

/**
 * Tear down a registry, releasing every chunk and both LUT reservations.
 */
static inline void
dmamem_registry_term(struct dmamem_registry *registry)
{
	if (!registry) {
		return;
	}

	dmamem_registry_clear(registry);

	if (registry->lut_phys) {
		munmap(registry->lut_phys, registry->lut_capacity * sizeof(*registry->lut_phys));
		registry->lut_phys = NULL;
	}
	if (registry->lut_meta) {
		munmap(registry->lut_meta, registry->lut_capacity * sizeof(*registry->lut_meta));
		registry->lut_meta = NULL;
	}
	registry->lut_capacity = 0;
}

/**
 * Shared body of add/adopt: bump the chunks covering [vaddr, vaddr + nbytes),
 * populating or adopting the ones that were not live, and record the
 * registration.
 *
 * When `adopt_lut` is non-NULL the chunk addresses are taken from it, indexed
 * by (chunk_va - vaddr) >> adopt_shift, and the chunks are marked borrowed.
 */
static inline int
dmamem_registry_add_impl(struct dmamem_registry *registry, void *vaddr, size_t nbytes,
			 const uint64_t *adopt_lut, int adopt_shift,
			 struct dmamem_registry_registration **out)
{
	uint64_t va;
	size_t chunk_first, chunk_cnt, bumped_cnt = 0;
	struct dmamem_registry_registration *m = NULL;
	int err;

	if (!registry || !registry->lut_phys || !vaddr || !nbytes) {
		return -EINVAL;
	}

	const uint64_t mask = registry->gran_mask;
	const int gran_shift = registry->gran_shift;

	va = (uint64_t)vaddr;
	chunk_first = (size_t)(va >> gran_shift);
	chunk_cnt = (size_t)(((va & mask) + nbytes + mask) >> gran_shift);

	if (chunk_first + chunk_cnt > registry->lut_capacity) {
		UPCIE_DEBUG("FAILED: range exceeds LUT capacity; raise va_bits at init");
		return -EINVAL;
	}

	m = calloc(1, sizeof(*m));
	if (!m) {
		return -ENOMEM;
	}
	m->vaddr = va;
	m->size = nbytes;

	for (size_t k = 0; k < chunk_cnt; ++k) {
		const size_t idx = chunk_first + k;
		struct dmamem_registry_chunk_meta *cm = &registry->lut_meta[idx];

		if (cm->rc == 0) {
			const uint64_t chunk_va = (uint64_t)idx << gran_shift;

			if (adopt_lut) {
				const uint64_t base = va & ~mask;

				registry->lut_phys[idx] =
					adopt_lut[(chunk_va - base) >> adopt_shift];
				cm->borrowed = 1;
			} else {
				err = registry->populate(registry->ctx, chunk_va,
							 (size_t)mask + 1,
							 &registry->lut_phys[idx], &cm->attach);
				if (err) {
					goto err_unwind;
				}
			}
		}
		cm->rc++;
		bumped_cnt++;
	}

	m->next = registry->list;
	registry->list = m;
	if (out) {
		*out = m;
	}

	return 0;

err_unwind:
	dmamem_registry_chunk_deref(registry, chunk_first, bumped_cnt);
	free(m);

	return err;
}

/**
 * Register a range, discovering the addresses of chunks not already live.
 *
 * `vaddr` and `nbytes` may have any byte alignment; the chunk cache resolves
 * at byte granularity. Consumers may impose more, e.g. NVMe PRP construction
 * wants host-page-aligned buffers.
 *
 * @return 0 on success, negative errno on failure. -EINVAL when the range
 *         exceeds the LUT capacity chosen at init.
 */
static inline int
dmamem_registry_add(struct dmamem_registry *registry, void *vaddr, size_t nbytes,
		    struct dmamem_registry_registration **out)
{
	return dmamem_registry_add_impl(registry, vaddr, nbytes, NULL, 0, out);
}

/**
 * Register a range whose addresses the caller already knows.
 *
 * `lut` holds addresses for the range starting at the chunk containing
 * `vaddr`, one entry per `1 << lut_shift` bytes, which must be no coarser
 * than the registry's granularity. Nothing is discovered and nothing is
 * released; the caller keeps ownership of whatever produced the addresses and
 * must outlive the registration.
 *
 * @return 0 on success, negative errno on failure.
 */
static inline int
dmamem_registry_adopt(struct dmamem_registry *registry, void *vaddr, size_t nbytes,
		      const uint64_t *lut, int lut_shift,
		      struct dmamem_registry_registration **out)
{
	if (!lut || lut_shift > registry->gran_shift) {
		return -EINVAL;
	}

	return dmamem_registry_add_impl(registry, vaddr, nbytes, lut, lut_shift, out);
}

/**
 * Remove the registration starting at `vaddr`.
 *
 * @return 0 on success, -EINVAL when no registration starts there.
 */
static inline int
dmamem_registry_remove(struct dmamem_registry *registry, void *vaddr)
{
	if (!registry) {
		return -EINVAL;
	}

	const uint64_t key = (uint64_t)vaddr;
	const int gran_shift = registry->gran_shift;
	const uint64_t mask = registry->gran_mask;

	for (struct dmamem_registry_registration **prev = &registry->list, *m = registry->list; m;
	     prev = &m->next, m = m->next) {
		if (m->vaddr != key) {
			continue;
		}

		const size_t chunk_first = (size_t)(m->vaddr >> gran_shift);
		const size_t chunk_cnt =
			(size_t)(((m->vaddr & mask) + m->size + mask) >> gran_shift);

		dmamem_registry_chunk_deref(registry, chunk_first, chunk_cnt);

		*prev = m->next;
		free(m);

		return 0;
	}

	return -EINVAL;
}

/**
 * Resolve a registered virtual address. One load, whatever is registered.
 *
 * @return 0 on success, -EINVAL when `virt` is not in a live chunk.
 */
static inline int
dmamem_registry_virt_to_phys(struct dmamem_registry *registry, void *virt, uint64_t *phys)
{
	if (!registry || !virt || !phys) {
		return -EINVAL;
	}

	const uint64_t va = (uint64_t)virt;
	const size_t idx = (size_t)(va >> registry->gran_shift);
	uint64_t base;

	if (idx >= registry->lut_capacity) {
		return -EINVAL;
	}

	base = registry->lut_phys[idx];
	if (base == 0) {
		return -EINVAL;
	}

	*phys = base + (va & registry->gran_mask);

	return 0;
}
