#include <hostmem.h>
#include <hostmem_dma.h>
#include <hostmem_heap.h>

int
main(int argc, const char *argv[])
{
	const size_t sizes[] = {1024 * 1024, 1024 * 1024 * 2ULL};
	void *buf;

	for (int i = 0; i < sizeof(sizes) / sizeof(*sizes); ++i) {
		size_t nbytes = sizes[i];

		buf = hostmem_dma_malloc(nbytes);
		if (!buf) {
			printf("hostmem_dma_malloc(%zu)\n", nbytes);
			return -errno;
		}

		hostmem_dma_free(buf);
	}

	return 0;
}
