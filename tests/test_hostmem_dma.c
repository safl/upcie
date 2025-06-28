#include <upcie/upcie.h>

#define HOSTMEM_HEAP_SIZE (1024 * 1024 * 512ULL)

int
main(void)
{
	struct hostmem_config config = {0};
	struct hostmem_heap heap = {0};
	const size_t sizes[] = {1024, 1024 * 1024, 1024 * 1024 * 2ULL};
	void *buf;
	int err;

	err = hostmem_config_init(&config);
	if (err) {
		return -err;
	}

	err = hostmem_heap_init(&heap, HOSTMEM_HEAP_SIZE, &config);
	if (err) {
		printf("hostmem_dma_init(); err(%d)\n", -err);
		printf("Check status: hugepages info\n");
		printf("Reserve 2G: hugepages setup --count 1024\n");
		return -err;
	}

	for (size_t i = 0; i < sizeof(sizes) / sizeof(*sizes); ++i) {
		size_t nbytes = sizes[i];

		buf = hostmem_dma_malloc(&heap, nbytes);
		if (!buf) {
			printf("hostmem_dma_malloc(%zu)\n", nbytes);
			return -errno;
		}

		hostmem_dma_free(&heap, buf);
	}

	hostmem_heap_term(&heap);

	return 0;
}
