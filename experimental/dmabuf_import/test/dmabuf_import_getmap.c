// SPDX-License-Identifier: GPL-2.0
/*
 * dmabuf_import_getmap - a stalled GET_MAP copy-out must not block the tree
 * =========================================================================
 *
 * DMABUF_IMPORT_GET_MAP used to call copy_to_user() per segment while still
 * holding the lock its import was found under. A fault on the destination can
 * sleep for as long as userspace makes it, and every other ioctl on the same
 * tree waited behind it. The fix gathers the segments into a kernel array
 * under the lock and copies them out after dropping it.
 *
 * This shows the difference with two threads on one open of the device, so
 * they contend on the same per-file lock. One thread issues GET_MAP with a
 * dma_arr placed on a userfaultfd-registered page whose fault nothing
 * resolves, so it stalls inside the copy; the header fields sit on an
 * ordinary page so only the copy-out faults. The userfaultfd message doubles
 * as the synchronisation point: once it arrives, the stalled thread holds the
 * lock on the unfixed module and has dropped it on the fixed one. The other
 * thread then issues DESCRIBE on a second import of its own; on the fixed
 * module it completes at once, on the unfixed one it waits until the fault is
 * resolved, which the timeout here calls a failure. Either way the main
 * thread then resolves the fault with UFFDIO_COPY, so everything terminates.
 *
 * The userfaultfd is created without UFFD_USER_MODE_ONLY on purpose: the
 * fault to be trapped is taken by the kernel inside copy_to_user(), not by
 * userspace. That needs root or vm.unprivileged_userfaultfd=1; run as root.
 *
 * Target-side (needs the stock /dev/udmabuf for UDMABUF_CREATE and the module
 * at /dev/dmabuf_import). No GPU. Exit 0 when DESCRIBE got through, 1 when it
 * sat behind the stalled copy, 2 when the test could not run at all.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/memfd.h>
#include <linux/udmabuf.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "../module/dmabuf_import.h"

#define PAGES 4

/* How long DESCRIBE gets before its waiting is called the bug. Generous: on
 * the fixed module it returns in microseconds. */
#define DESCRIBE_TIMEOUT_MS 10000

static int create_udmabuf(int udmabuf_fd, size_t size)
{
	struct udmabuf_create create;
	int memfd, dmabuf_fd;

	memfd = memfd_create("udmabuf-getmap", MFD_ALLOW_SEALING);
	if (memfd < 0) {
		perror("memfd_create");
		return -1;
	}
	if (fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK) || ftruncate(memfd, size)) {
		perror("seal/ftruncate");
		return -1;
	}

	memset(&create, 0, sizeof(create));
	create.memfd = memfd;
	create.offset = 0;
	create.size = size;

	dmabuf_fd = ioctl(udmabuf_fd, UDMABUF_CREATE, &create);
	close(memfd); /* the dma-buf holds what it needs of it */
	if (dmabuf_fd < 0) {
		perror("UDMABUF_CREATE");
		return -1;
	}
	return dmabuf_fd;
}

struct stall {
	int import_fd;
	struct dmabuf_import_get_map *get_map;
	long ret;
	int err;
};

static void *stall_in_get_map(void *arg)
{
	struct stall *s = arg;

	s->ret = ioctl(s->import_fd, DMABUF_IMPORT_GET_MAP, s->get_map);
	s->err = errno;
	return NULL;
}

struct probe {
	int import_fd;
	int dmabuf_fd;
	int pipe_wr;
	long ret;
	int err;
};

static void *probe_with_describe(void *arg)
{
	struct probe *p = arg;
	struct dmabuf_import_describe describe;

	memset(&describe, 0, sizeof(describe));
	describe.fd = p->dmabuf_fd;
	p->ret = ioctl(p->import_fd, DMABUF_IMPORT_DESCRIBE, &describe);
	p->err = errno;
	if (write(p->pipe_wr, "x", 1) != 1)
		perror("write done-pipe");
	return NULL;
}

/* An import via DMABUF_IMPORT_ATTACH_BDF (empty bdf), on `import_fd`, so both
 * imports land in the same per-file tree. Returns the dma-buf fd, -1 on error. */
static int import_one(int udmabuf_fd, int import_fd)
{
	struct dmabuf_import_attach_bdf attach;
	int dmabuf_fd;

	dmabuf_fd = create_udmabuf(udmabuf_fd, (size_t)PAGES * 4096);
	if (dmabuf_fd < 0)
		return -1;

	memset(&attach, 0, sizeof(attach));
	attach.fd = dmabuf_fd;
	if (ioctl(import_fd, DMABUF_IMPORT_ATTACH_BDF, &attach)) {
		perror("DMABUF_IMPORT_ATTACH_BDF");
		return -1;
	}
	return dmabuf_fd;
}

int main(void)
{
	struct uffdio_register reg;
	struct uffdio_api api;
	struct uffdio_copy copy;
	struct uffd_msg msg;
	struct pollfd pfd;
	struct stall stall;
	struct probe probe;
	pthread_t stall_thread, probe_thread;
	struct dmabuf_import_get_map *get_map;
	unsigned char *area, *zeros;
	long pagesz = sysconf(_SC_PAGESIZE);
	int udmabuf_fd, import_fd, stall_buf, probe_buf, uffd, done_pipe[2];
	int described_in_time;

	/* Last resort: whatever goes wrong, exit_group() delivers a fatal
	 * signal to a thread parked in handle_userfault, so nothing lingers. */
	alarm(60);

	udmabuf_fd = open("/dev/udmabuf", O_RDWR);
	if (udmabuf_fd < 0) {
		perror("open /dev/udmabuf");
		return 2;
	}
	import_fd = open(DMABUF_IMPORT_DEVPATH, O_RDWR);
	if (import_fd < 0) {
		perror("open " DMABUF_IMPORT_DEVPATH);
		return 2;
	}

	stall_buf = import_one(udmabuf_fd, import_fd);
	probe_buf = import_one(udmabuf_fd, import_fd);
	if (stall_buf < 0 || probe_buf < 0)
		return 2;

	uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
	if (uffd < 0) {
		if (errno == ENOSYS)
			fprintf(stderr, "SKIP: no userfaultfd(2) in this kernel\n");
		else if (errno == EPERM)
			fprintf(stderr, "SKIP: userfaultfd(2) refused; run as root or set"
					" /proc/sys/vm/unprivileged_userfaultfd\n");
		else
			perror("userfaultfd");
		return 2;
	}

	memset(&api, 0, sizeof(api));
	api.api = UFFD_API;
	if (ioctl(uffd, UFFDIO_API, &api)) {
		perror("UFFDIO_API");
		return 2;
	}

	/* Two pages: get_map's header goes at the end of the first, ordinary
	 * one, so copy_from_user() of it does not fault; dma_arr then starts
	 * exactly at the second, userfaultfd-registered one, so the copy-out
	 * is what stalls. */
	area = mmap(NULL, 2 * pagesz, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	zeros = mmap(NULL, pagesz, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (area == MAP_FAILED || zeros == MAP_FAILED) {
		perror("mmap");
		return 2;
	}

	memset(&reg, 0, sizeof(reg));
	reg.range.start = (unsigned long)(area + pagesz);
	reg.range.len = pagesz;
	reg.mode = UFFDIO_REGISTER_MODE_MISSING;
	if (ioctl(uffd, UFFDIO_REGISTER, &reg)) {
		perror("UFFDIO_REGISTER");
		return 2;
	}

	get_map = (struct dmabuf_import_get_map
			   *)(area + pagesz - offsetof(struct dmabuf_import_get_map, dma_arr));
	get_map->fd = stall_buf;
	get_map->count = 1; /* one faulting entry is stall enough */

	memset(&stall, 0, sizeof(stall));
	stall.import_fd = import_fd;
	stall.get_map = get_map;
	if (pthread_create(&stall_thread, NULL, stall_in_get_map, &stall)) {
		perror("pthread_create");
		return 2;
	}

	/* The fault message says the stalled thread is inside copy_to_user():
	 * on the unfixed module that is under the per-file lock, on the fixed
	 * one past it. Without the message GET_MAP never reached the copy. */
	pfd.fd = uffd;
	pfd.events = POLLIN;
	if (poll(&pfd, 1, DESCRIBE_TIMEOUT_MS) != 1) {
		pthread_join(stall_thread, NULL);
		fprintf(stderr, "ERROR: no page fault arrived; GET_MAP returned %ld"
				" (errno %d) without touching dma_arr\n",
			stall.ret, stall.err);
		return 2;
	}
	if (read(uffd, &msg, sizeof(msg)) != sizeof(msg) ||
	    msg.event != UFFD_EVENT_PAGEFAULT) {
		fprintf(stderr, "ERROR: unexpected userfaultfd message\n");
		return 2;
	}

	if (pipe(done_pipe)) {
		perror("pipe");
		return 2;
	}
	memset(&probe, 0, sizeof(probe));
	probe.import_fd = import_fd;
	probe.dmabuf_fd = probe_buf;
	probe.pipe_wr = done_pipe[1];
	if (pthread_create(&probe_thread, NULL, probe_with_describe, &probe)) {
		perror("pthread_create");
		return 2;
	}

	pfd.fd = done_pipe[0];
	pfd.events = POLLIN;
	described_in_time = poll(&pfd, 1, DESCRIBE_TIMEOUT_MS) == 1;

	/* Let the stalled copy finish, whatever was observed: this is what
	 * releases the lock on the unfixed module, so DESCRIBE completes and
	 * both threads can be joined. */
	memset(&copy, 0, sizeof(copy));
	copy.dst = (unsigned long)(area + pagesz);
	copy.src = (unsigned long)zeros;
	copy.len = pagesz;
	if (ioctl(uffd, UFFDIO_COPY, &copy) && errno != EEXIST) {
		perror("UFFDIO_COPY");
		return 2;
	}

	pthread_join(stall_thread, NULL);
	pthread_join(probe_thread, NULL);

	if (!described_in_time) {
		fprintf(stderr, "FAIL: DESCRIBE of an unrelated import sat %d ms behind a"
				" GET_MAP stalled in its copy-out\n",
			DESCRIBE_TIMEOUT_MS);
		return 1;
	}

	if (probe.ret) {
		fprintf(stderr, "ERROR: DESCRIBE failed with errno %d\n", probe.err);
		return 2;
	}
	if (stall.ret) {
		fprintf(stderr, "ERROR: GET_MAP failed with errno %d after the fault"
				" was resolved\n",
			stall.err);
		return 2;
	}

	printf("DESCRIBE completed while GET_MAP was stalled in its copy-out\n");

	close(import_fd); /* gives both imports back */

	return 0;
}
