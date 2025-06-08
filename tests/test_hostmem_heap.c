#include <hostmem.h>

int
main(int argc, const char *argv[])
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

	err = hostmem_heap_init(&g_hostmem_state.heap, 1024 * 1024 * 256ULL);
	if (err) {
		printf("# FAILED: hostmem_heap_init(); err(%d)\n", err);
		return -err;
	}

	hostmem_heap_pp(&g_hostmem_state.heap);

	for (int i = 0; i < nbuffers; i++) {

		buffers[i] = hostmem_buffer_alloc(&g_hostmem_state.heap, 4);
		if (!buffers[i]) {
			return -errno;
		}
	}

	hostmem_heap_pp(&g_hostmem_state.heap);

	for (int i = 0; i < nbuffers; i++) {
		hostmem_buffer_free(&g_hostmem_state.heap, buffers[i]);
	}

	hostmem_heap_pp(&g_hostmem_state.heap);

	hostmem_heap_term(&g_hostmem_state.heap);

	return 0;
}
