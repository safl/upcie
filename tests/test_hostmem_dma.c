#include <upcie/upcie.h>

#define HOSTMEM_HEAP_SIZE (1024 * 1024 * 512ULL)

int
main(void)
{
	const size_t sizes[] = {1024, 1024 * 1024, 1024 * 1024 * 2ULL};
	void *buf;
	int err;

	err = hostmem_dma_init(HOSTMEM_HEAP_SIZE);
	if (err) {
		printf("hostmem_dma_init(); err(%d)\n", -err);
		printf("Check status: hugepages info\n");
		printf("Reserve 2G: hugepages setup --count 1024\n");
		return -err;
	}

	for (size_t i = 0; i < sizeof(sizes) / sizeof(*sizes); ++i) {
		size_t nbytes = sizes[i];

		buf = hostmem_dma_malloc(nbytes);
		if (!buf) {
			printf("hostmem_dma_malloc(%zu)\n", nbytes);
			return -errno;
		}

		hostmem_dma_free(buf);
	}

	hostmem_dma_term();

	return 0;
}
