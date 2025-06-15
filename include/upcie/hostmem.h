// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
  Memory allocator using hugepages for DMA in user-space drivers and host IPC
  ===========================================================================

  The library provides two sets of APIs for host memory. The first, a foundation for interaction
  with hugepages. The second, a malloc-like buffer-allocator which makes use of the first API for
  its backing memory, for the purpose of DMA.

  API: Hugepages
  --------------

  * hostmem_hugepage_init()
    - Checks environment variables and sets up state

  * hostmem_hugepage_alloc()
    - Allocate memory in multiples of hugepage size

  * hostmem_hugepage_import()
    - Import hugepage for allocated by another process

  * hostmem_hugepage_free()
    - De-allocate memory obtained with with hostmem_hugepage_{alloc,import}()

  Caveat: system setup
  --------------------

  The library makes use of memfd_create(MFD_HUGETLB), however, you still need to allocate them
  yourself. That is, have a system setup step than makes hugepages available, such as:

    echo 128 | tee -a /proc/sys/vm/nr_hugepages
    ulimit -l unlimited

  Thus, a utility for this similar to devbind.py is needed. This is what we have today with
  'xnvme-driver', however, we want something simpler.

  Caveat: CAP_SYS_ADMIN
  ---------------------

  Reading /proc/self/pagemap requires CAP_SYS_ADMIN, so hostmem_virt_to_phys() cannot be used by
  non-privileged users. Therefore, any process needing DMA via this allocator must run as root.

  Possible Workaround: Since the allocator uses MAP_SHARED, a privileged "allocator-daemon" could
  handle virt_to_phys translations and share the results via shared memory with unprivileged
  clients. This allows integration into the heap with minimal complexity. Example:

  After heap initialization, write the heap structure into hugepage memory. Because phys_lut[]
  resolves all physical addresses of the backing hugepages, any process that imports the hugepage
  also gains access to those physical addresses—without needing CAP_SYS_ADMIN.

  @file hostmem.h
*/
#ifndef MFD_HUGE_2MB
#define MFD_HUGE_2MB (21 << 26)
#endif
#ifndef MFD_HUGE_1GB
#define MFD_HUGE_1GB (30 << 26)
#endif

enum hostmem_backend {
	HOSTMEM_BACKEND_UNKNOWN = 0x0,
	HOSTMEM_BACKEND_MEMFD = 0x1,
	HOSTMEM_BACKEND_HUGETLBFS = 0x2,
};

struct hostmem_hugepage {
	int fd;
	void *virt;
	size_t size;
	uint64_t phys;
	char path[256];
};

static inline int
hostmem_hugepage_pp(struct hostmem_hugepage *hugepage)
{
	int wrtn = 0;

	wrtn += printf("hostmem_hugepage:");

	if (!hugepage) {
		wrtn += printf(" ~\n");
		return 0;
	}

	wrtn += printf("\n");
	wrtn += printf("  fd: %d\n", hugepage->fd);
	wrtn += printf("  size: %zu\n", hugepage->size);
	wrtn += printf("  virt: 0x%" PRIx64 "\n", (uint64_t)hugepage->virt);
	wrtn += printf("  phys: 0x%" PRIx64 "\n", hugepage->phys);
	wrtn += printf("  path: '%.*s'\n", 256, hugepage->path);

	return wrtn;
};

/**
 * A representation of a various host memory properties, primarly for hugepages
 */
struct hostmem_state {
	char hugetlb_path[128]; ///< Mountpoint of hugetlbsfs
	int memfd_flags;	///< Flags for memfd_create(...)
	int backend;
	int count;
	int pagesize;		  ///< Host memory pagesize (not HUGEPAGE size)
	int hugepgsz;		  ///< THIS, is the HUGEPAGE size
};

/**
 * Global default state of the host memory allocate
 */
static struct hostmem_state g_hostmem_state = {
    .hugetlb_path = "/mnt/huge",
    .backend = HOSTMEM_BACKEND_UNKNOWN,
    .count = 0,
};

static inline int
hostmem_state_pp(struct hostmem_state *state)
{
	int wrtn = 0;

	wrtn += printf("hostmem:");

	if (!state) {
		wrtn += printf(" ~\n");
		return 0;
	}

	wrtn += printf("  \n");
	wrtn += printf("  hugetlb_path: '%s'\n", state->hugetlb_path);
	wrtn += printf("  memfd_flags: 0x%x\n", state->memfd_flags);
	wrtn += printf("  backend: 0x%x\n", state->backend);
	wrtn += printf("  count: %d\n", state->count);
	wrtn += printf("  pagesize: %d\n", state->pagesize);
	wrtn += printf("  hugepgsz: %d\n", state->hugepgsz);

	return wrtn;
};

static inline int
hostmem_state_get_hugepgsz(int *hugepgsz)
{
	char line[256];
	FILE *fp;

	fp = fopen("/proc/meminfo", "r");
	if (!fp) {
		perror("fopen(meminfo)");
		return -errno;
	}

	while (fgets(line, sizeof(line), fp)) {
		if (strncmp(line, "Hugepagesize:", 13) == 0) {
			int kb;
			if (sscanf(line + 13, "%d", &kb) == 1) {
				fclose(fp);
				*hugepgsz = kb * 1024;
				return 0;
			}
		}
	}

	fclose(fp);
	return -ENOMEM;
}

static inline int
hostmem_state_init(struct hostmem_state *state)
{
	const char *env;
	int err;

	state->pagesize = getpagesize();

	err = hostmem_state_get_hugepgsz(&state->hugepgsz);
	if (err) {
		return err;
	}

	state->memfd_flags = MFD_HUGETLB;
	if (state->hugepgsz == 2 * 1024 * 1024) {
		state->memfd_flags |= MFD_HUGE_2MB;
	} else if (state->hugepgsz == 1 * 1024 * 1024 * 1024) {
		state->memfd_flags |= MFD_HUGE_1GB;
	} else {
		fprintf(stderr, "Unsupported hugepgsz(%d)\n", state->hugepgsz);
		return -EINVAL;
	}

	env = getenv("HOSTMEM_HUGETLB_PATH");
	if (env) {
		snprintf(state->hugetlb_path, sizeof(state->hugetlb_path), "%s", env);
	}

	env = getenv("HOSTMEM_BACKEND");
	if (env) {
		if (env && strcmp(env, "memfd") == 0) {
			state->backend = HOSTMEM_BACKEND_MEMFD;
		} else if (env && strcmp(env, "hugetlbfs") == 0) {
			state->backend = HOSTMEM_BACKEND_HUGETLBFS;
		} else {
			return -EINVAL;
		}
	} else {
		state->backend = HOSTMEM_BACKEND_MEMFD;
	}

	return 0;
}

static inline int
hostmem_internal_memfd_create(const char *name, unsigned int flags)
{
	return syscall(SYS_memfd_create, name, flags);
}

/**
 * Consult "/proc/self/pagemap" for the given va-space address
 *
 * NOTE: The implementation assumes 55bit VA-space, is this assumption safe?
 *
 * @param virt The address in the process virtual-address space to resolve the
 * physical address for
 * @param phys Pointer to where the physical address should be recorded
 *
 * @returns On success, 0 is returned. On error, negative errno is return to
 * indicate the error.
 *
 */
static inline int
hostmem_pagemap_virt_to_phys(void *virt, uint64_t *phys)
{
	const int pagemap_entry_bytes = 8;
	const uint64_t pfn_mask = ((1ULL << 55) - 1);
	uint64_t entry = 0;
	uint64_t virt_pfn = (uint64_t)virt / getpagesize();
	uint64_t phys_pfn;
	int fd;

	fd = open("/proc/self/pagemap", O_RDONLY);
	if (fd < 0) {
		perror("open(pagemap)");
		return -errno;
	}

	if (pread(fd, &entry, pagemap_entry_bytes, virt_pfn * pagemap_entry_bytes) !=
	    pagemap_entry_bytes) {
		perror("pread(pagemap)");
		close(fd);
		return -errno;
	}

	if (!(entry & (1ULL << 63))) {
		fprintf(stderr, "Page not present\n");
		close(fd);
		return -EINVAL;
	}

	phys_pfn = entry & pfn_mask;
	*phys = (phys_pfn * getpagesize()) + ((uint64_t)virt % getpagesize());

	close(fd);

	return 0;
}

/**
 * Deallocate a hugepage allocation
 */
static inline void
hostmem_hugepage_free(struct hostmem_hugepage *hugepage)
{
	if (!hugepage)
		return;

	if (hugepage->virt && hugepage->size) {
		munmap(hugepage->virt, hugepage->size);
	}

	if (HOSTMEM_BACKEND_HUGETLBFS == g_hostmem_state.backend) {
		unlink(hugepage->path);
	}

	memset(hugepage, 0, sizeof(*hugepage));
}

/**
 * Allocate a hugepage of the given 'size'
 *
 * @param size Must be a multiple of 2M
 * @param hugepage Pointer to a pre-allocated hugepage-descriptor
 *
 * @return On success, 0 is returned. On error, negative errno is returned to
 * indicate the error.
 */
static inline int
hostmem_hugepage_alloc(size_t size, struct hostmem_hugepage *hugepage)
{
	int err;

	if (size % (g_hostmem_state.hugepgsz) != 0) {
		fprintf(stderr, "size must be multiple of hugepgsz(%d)\n",
			g_hostmem_state.hugepgsz);
		return -EINVAL;
	}

	hugepage->size = size;

	switch (g_hostmem_state.backend) {
	case HOSTMEM_BACKEND_MEMFD:
		hugepage->fd =
		    hostmem_internal_memfd_create("hostmem", g_hostmem_state.memfd_flags);
		if (hugepage->fd < 0) {
			perror("memfd_create()");
			return -errno;
		}

		snprintf(hugepage->path, sizeof(hugepage->path), "/proc/%d/fd/%d", getpid(),
			 hugepage->fd);
		break;

	case HOSTMEM_BACKEND_HUGETLBFS:
		snprintf(hugepage->path, sizeof(hugepage->path), "%s/%d",
			 g_hostmem_state.hugetlb_path, g_hostmem_state.count);

		hugepage->fd = open(hugepage->path, O_CREAT | O_RDWR, 0600);
		if (hugepage->fd < 0) {
			perror("open(hugepage)");
			return -errno;
		}

		break;
	}

	if (ftruncate(hugepage->fd, hugepage->size) != 0) {
		perror("ftruncate(hugepage)");
		close(hugepage->fd);
		return -ENOMEM;
	}

	hugepage->virt =
	    mmap(NULL, hugepage->size, PROT_READ | PROT_WRITE, MAP_SHARED, hugepage->fd, 0);
	if (hugepage->virt == MAP_FAILED) {
		perror("mmap(hugepage)");
		close(hugepage->fd);
		return -ENOMEM;
	}

	err = mlock(hugepage->virt, hugepage->size);
	if (err) {
		perror("mlock(hugepage)");
		munmap(hugepage->virt, hugepage->size);
		close(hugepage->fd);
		return -ENOMEM;
	}

	{
		volatile char *ptr = (volatile char *)hugepage->virt;
		for (size_t i = 0; i < hugepage->size; i += g_hostmem_state.pagesize) {
			ptr[i] = 0;
		}
	}

	///< The assumption here is that memset and mlock should lead to pinned pages
	memset(hugepage->virt, 0, hugepage->size);

	err = hostmem_pagemap_virt_to_phys(hugepage->virt, &hugepage->phys);
	if (err) {
		perror("hostmem_virt_to_phys(hugepage)");
		munmap(hugepage->virt, hugepage->size);
		close(hugepage->fd);
		return -ENOMEM;
	}

	g_hostmem_state.count++;

	return 0;
}

/**
 * Import (re-map) an existing hugepage shared by another process.
 *
 * This function uses fstat() to determine the size of the shared memory region.
 *
 * @param path Path to the memfd or hugetlbfs file (e.g. /proc/<pid>/fd/<fd>)
 * @param hugepage Pre-allocated pointer to the descriptor to fill in
 *
 * @return 0 on success, negative errno on error
 */
static inline int
hostmem_hugepage_import(const char *path, struct hostmem_hugepage *hugepage)
{
	struct stat st;
	int err;

	if (!path || !hugepage) {
		return -EINVAL;
	}

	snprintf(hugepage->path, sizeof(hugepage->path), "%s", path);

	hugepage->fd = open(hugepage->path, O_RDWR);
	if (hugepage->fd < 0) {
		perror("open(hugepage_import_path)");
		return -errno;
	}

	if (fstat(hugepage->fd, &st) != 0) {
		perror("fstat(hugepage_import_path)");
		close(hugepage->fd);
		return -errno;
	}
	hugepage->size = st.st_size;

	if (hugepage->size % g_hostmem_state.hugepgsz != 0) {
		fprintf(stderr, "Error: mapped file size (%zu) is not hugepgsz(%d) aligned\n",
			st.st_size, g_hostmem_state.hugepgsz);
		close(hugepage->fd);
		return -EINVAL;
	}

	hugepage->virt =
	    mmap(NULL, hugepage->size, PROT_READ | PROT_WRITE, MAP_SHARED, hugepage->fd, 0);
	if (hugepage->virt == MAP_FAILED) {
		perror("mmap(import)");
		close(hugepage->fd);
		return -errno;
	}

	// This is done to ensure that pages are 'paged-in', that is, the process
	// importing the hugepage will be able to resolve virtual pages to physical
	// ones, without this, then this mapping will not be done
	{
		volatile const char *p = (volatile const char *)hugepage->virt;
		volatile char sink;
		for (size_t i = 0; i < hugepage->size; i += g_hostmem_state.pagesize) {
			sink = p[i];
			(void)sink; ///< Avoid compiler-warnings "unused-but-set-variable"
		}
	}

	err = hostmem_pagemap_virt_to_phys(hugepage->virt, &hugepage->phys);
	if (err) {
		fprintf(stderr, "Failed to resolve physical address\n");
		munmap(hugepage->virt, st.st_size);
		close(hugepage->fd);
		return -ENOMEM;
	}

	return 0;
}
