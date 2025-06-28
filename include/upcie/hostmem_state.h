#ifndef MFD_HUGE_2MB
#define MFD_HUGE_2MB (21 << 26)
#endif
#ifndef MFD_HUGE_1GB
#define MFD_HUGE_1GB (30 << 26)
#endif

/**
 * A representation of a various host memory properties, primarly for hugepages
 */
struct hostmem_state {
	char hugetlb_path[128]; ///< Mountpoint of hugetlbsfs
	int memfd_flags;	///< Flags for memfd_create(...)
	int backend;
	int count;
	int pagesize; ///< Host memory pagesize (not HUGEPAGE size)
	int hugepgsz; ///< THIS, is the HUGEPAGE size
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

	sprintf(state->hugetlb_path, "/mnt/huge");
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
