#include <upcie/upcie.h>

int
main(void)
{
	struct hostmem_heap heap = {0};
	const size_t nbuffers = 10;
	void *buffers[nbuffers];
	int err;

	err = hostmem_state_init(&g_hostmem_state);
	if (err) {
		printf("# FAILED: hostmem_state_init(); err(%d)\n", err);
		return -err;
	}
	hostmem_state_pp(&g_hostmem_state);

	err = hostmem_heap_init(&heap, 1024 * 1024 * 256ULL);
	if (err) {
		printf("# FAILED: hostmem_heap_init(); err(%d)\n", err);
		return -err;
	}

	hostmem_heap_pp(&heap);

	for (size_t i = 0; i < nbuffers; i++) {

		buffers[i] = hostmem_heap_block_alloc(&heap, 4);
		if (!buffers[i]) {
			return -errno;
		}
	}

	hostmem_heap_pp(&heap);

	for (size_t i = 0; i < nbuffers; i++) {
		hostmem_heap_block_free(&heap, buffers[i]);
	}

	hostmem_heap_pp(&heap);

	hostmem_heap_term(&heap);

	return 0;
}
