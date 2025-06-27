#include <upcie/upcie.h>

int
main(void)
{
	const size_t nbuffers = 10;
	void *buffers[nbuffers];
	int err;

	err = hostmem_state_init(&g_hostmem_state);
	if (err) {
		printf("# FAILED: hostmem_state_init(); err(%d)\n", err);
		return -err;
	}

	hostmem_state_pp(&g_hostmem_state);

	err = hostmem_heap_init(&g_hostmem_heap_dma, 1024 * 1024 * 256ULL);
	if (err) {
		printf("# FAILED: hostmem_heap_init(); err(%d)\n", err);
		return -err;
	}

	hostmem_heap_pp(&g_hostmem_heap_dma);

	for (size_t i = 0; i < nbuffers; i++) {

		buffers[i] = hostmem_heap_block_alloc(&g_hostmem_heap_dma, 4);
		if (!buffers[i]) {
			return -errno;
		}
	}

	hostmem_heap_pp(&g_hostmem_heap_dma);

	for (size_t i = 0; i < nbuffers; i++) {
		hostmem_heap_block_free(&g_hostmem_heap_dma, buffers[i]);
	}

	hostmem_heap_pp(&g_hostmem_heap_dma);

	hostmem_heap_term(&g_hostmem_heap_dma);

	return 0;
}
