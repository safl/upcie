// SPDX-License-Identifier: GPL-2.0
/*
 * dmabuf_import_owner - two processes, one descriptor number, two buffers
 * ======================================================================
 *
 * DMABUF_IMPORT_ATTACH keys its imports by the dma-buf descriptor number in a
 * table shared by every user of the device. A descriptor number means something
 * only inside the process that holds it, so two processes naming different
 * buffers with the same number collide; the module used to answer the second
 * with the first's buffer, and now refuses it with ESTALE.
 *
 * DMABUF_IMPORT_ATTACH_BDF hands the import to the file it was made on, so the
 * numbers cannot meet. Both are exercised here with the same collision.
 *
 * What is compared is the byte count rather than the segment count. udmabuf
 * merges pages and the mapping may coalesce further, so how many segments a
 * buffer arrives as depends on where its pages landed; how many bytes it is
 * does not.
 *
 * The two are asserted differently on purpose. The bdf form must answer the
 * second process with its own buffer, and fails this test when it does not.
 * The old form may refuse the collision, which is what it does today, or
 * answer with the caller's own buffer, which a rework might; what fails this
 * test is the one thing it must never do again, answering with the other
 * process's.
 *
 * Target-side (needs the stock /dev/udmabuf for UDMABUF_CREATE and the module
 * at /dev/dmabuf_import). Exit 0 on OK, 1 when either form hands a process
 * another buffer's addresses.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/memfd.h>
#include <linux/udmabuf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../module/dmabuf_import.h"

/* Both processes put their dma-buf here, so the two differ in nothing the
 * shared table could tell apart. */
#define SHARED_FD 100

#define SMALL_PAGES 4
#define LARGE_PAGES 32

static int create_udmabuf(int udmabuf_fd, size_t size)
{
	struct udmabuf_create create;
	int memfd, dmabuf_fd;

	memfd = memfd_create("udmabuf-owner", MFD_ALLOW_SEALING);
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

/* A descriptor closed without a DETACH leaves its import behind, and the
 * number can then be given to another buffer. Answering for the new buffer
 * with the old import would hand out the wrong addresses, so it is refused;
 * nothing is torn down, since the old import's addresses may still be in a
 * device. Returns 0 when refused as it should be, 1 when it answered anyway. */
static int reused_number_is_refused(int udmabuf_fd)
{
	struct dmabuf_import_attach_bdf attach;
	struct dmabuf_import_describe describe;
	int import_fd, small, large;

	import_fd = open(DMABUF_IMPORT_DEVPATH, O_RDWR);
	if (import_fd < 0) {
		perror("open /dev/dmabuf_import");
		return -1;
	}

	small = create_udmabuf(udmabuf_fd, (size_t)SMALL_PAGES * 4096);
	if (small < 0 || dup2(small, SHARED_FD) < 0) {
		return -1;
	}
	close(small);

	memset(&attach, 0, sizeof(attach));
	attach.fd = SHARED_FD;
	if (ioctl(import_fd, DMABUF_IMPORT_ATTACH_BDF, &attach)) {
		perror("attach the first buffer");
		return -1;
	}

	close(SHARED_FD); /* no DETACH: the import stays, as it is meant to */

	large = create_udmabuf(udmabuf_fd, (size_t)LARGE_PAGES * 4096);
	if (large < 0 || dup2(large, SHARED_FD) < 0) {
		return -1;
	}
	close(large);

	memset(&describe, 0, sizeof(describe));
	describe.fd = SHARED_FD;
	if (!ioctl(import_fd, DMABUF_IMPORT_DESCRIBE, &describe)) {
		fprintf(stderr,
			"FAIL: a re-used descriptor was answered with %llu pages,"
			" the buffer it now names is %d\n",
			(unsigned long long)(describe.nbytes / 4096), LARGE_PAGES);
		return 1;
	}
	if (errno != ESTALE) {
		fprintf(stderr, "FAIL: refused with errno %d, expected ESTALE\n", errno);
		return 1;
	}

	/* The same refusal on the way in, not only on the way out. */
	memset(&attach, 0, sizeof(attach));
	attach.fd = SHARED_FD;
	if (!ioctl(import_fd, DMABUF_IMPORT_ATTACH_BDF, &attach)) {
		fprintf(stderr, "FAIL: re-attach on a re-used descriptor was allowed\n");
		return 1;
	}
	if (errno != ESTALE) {
		fprintf(stderr, "FAIL: re-attach refused with errno %d, expected ESTALE\n",
			errno);
		return 1;
	}

	/* Detaching the import the number used to name settles it, and must work
	 * even though that descriptor is long gone. */
	if (ioctl(import_fd, DMABUF_IMPORT_DETACH, &(int){SHARED_FD})) {
		perror("detach the stale import");
		return 1;
	}

	memset(&attach, 0, sizeof(attach));
	attach.fd = SHARED_FD;
	if (ioctl(import_fd, DMABUF_IMPORT_ATTACH_BDF, &attach)) {
		perror("attach after detaching the stale import");
		return 1;
	}

	printf("re-used descriptor: refused until detached, as it should be\n");
	close(import_fd);

	return 0;
}

/* Import `pages` pages on SHARED_FD; returns the size in pages that the module
 * reports back for it, REFUSED when the module answered ESTALE, or -1 on
 * error. `owned` picks the ioctl. The device stays open, which the bdf form
 * requires and the old form does not mind. */
#define REFUSED (-2)

static int import_on_shared_fd(int pages, int owned)
{
	struct dmabuf_import_describe describe;
	struct dmabuf_import_attach_bdf bdf_attach;
	struct dmabuf_import_attach attach;
	int udmabuf_fd, import_fd, dmabuf_fd;

	udmabuf_fd = open("/dev/udmabuf", O_RDWR);
	if (udmabuf_fd < 0) {
		perror("open /dev/udmabuf");
		return -1;
	}
	import_fd = open(DMABUF_IMPORT_DEVPATH, O_RDWR);
	if (import_fd < 0) {
		perror("open /dev/dmabuf_import");
		return -1;
	}

	dmabuf_fd = create_udmabuf(udmabuf_fd, (size_t)pages * 4096);
	if (dmabuf_fd < 0)
		return -1;

	if (dup2(dmabuf_fd, SHARED_FD) < 0) {
		perror("dup2");
		return -1;
	}

	if (owned) {
		memset(&bdf_attach, 0, sizeof(bdf_attach));
		bdf_attach.fd = SHARED_FD;
		if (ioctl(import_fd, DMABUF_IMPORT_ATTACH_BDF, &bdf_attach)) {
			perror("DMABUF_IMPORT_ATTACH_BDF");
			return -1;
		}
	} else {
		memset(&attach, 0, sizeof(attach));
		attach.fd = SHARED_FD;
		if (ioctl(import_fd, DMABUF_IMPORT_ATTACH, &attach)) {
			if (errno == ESTALE)
				return REFUSED;
			perror("DMABUF_IMPORT_ATTACH");
			return -1;
		}
	}

	memset(&describe, 0, sizeof(describe));
	describe.fd = SHARED_FD;
	if (ioctl(import_fd, DMABUF_IMPORT_DESCRIBE, &describe)) {
		if (errno == ESTALE)
			return REFUSED;
		perror("DMABUF_IMPORT_DESCRIBE");
		return -1;
	}

	return (int)(describe.nbytes / 4096);
}

/* Parent imports SMALL_PAGES and holds it; child imports LARGE_PAGES on the
 * same descriptor number. Returns the count the child was given, REFUSED when
 * the module answered it ESTALE, or -1 on error. The page counts and these
 * two ride the child's exit status, so they must all stay under 254. */
#define EXIT_REFUSED 254
#define EXIT_ERROR   255

static int collide(int owned)
{
	int sync_pipe[2], parent_count, status;
	pid_t pid;
	char tok;

	if (pipe(sync_pipe)) {
		perror("pipe");
		return -1;
	}

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}

	if (pid == 0) {
		int count;

		close(sync_pipe[1]);
		if (read(sync_pipe[0], &tok, 1) != 1)
			_exit(EXIT_ERROR);

		count = import_on_shared_fd(LARGE_PAGES, owned);
		if (count == REFUSED)
			_exit(EXIT_REFUSED);
		_exit(count < 0 ? EXIT_ERROR : count);
	}

	close(sync_pipe[0]);

	parent_count = import_on_shared_fd(SMALL_PAGES, owned);
	if (parent_count != SMALL_PAGES) {
		fprintf(stderr, "parent: got %d pages, expected %d\n", parent_count,
			SMALL_PAGES);
		return -1;
	}

	if (write(sync_pipe[1], "x", 1) != 1)
		return -1;
	if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status))
		return -1;
	if (WEXITSTATUS(status) == EXIT_ERROR)
		return -1;

	/* The legacy form keeps its import until told otherwise, and this
	 * process is about to end without telling it. Left behind, it pins the
	 * buffer until the module goes, and stales the descriptor number for
	 * whoever names it next. */
	if (!owned) {
		int import_fd = open(DMABUF_IMPORT_DEVPATH, O_RDWR);

		if (import_fd >= 0) {
			ioctl(import_fd, DMABUF_IMPORT_DETACH, &(int){SHARED_FD});
			close(import_fd);
		}
	}

	if (WEXITSTATUS(status) == EXIT_REFUSED)
		return REFUSED;

	return WEXITSTATUS(status);
}

int main(void)
{
	int owned_count, legacy_count, udmabuf_fd, stale;

	udmabuf_fd = open("/dev/udmabuf", O_RDWR);
	if (udmabuf_fd < 0) {
		perror("open /dev/udmabuf");
		return 2;
	}

	stale = reused_number_is_refused(udmabuf_fd);
	if (stale < 0) {
		return 2;
	}
	close(udmabuf_fd);

	/* REFUSED falls through: the bdf form has no collision to refuse, so a
	 * refusal there is the assertion below failing, not a test error. */
	owned_count = collide(1);
	if (owned_count == -1)
		return 2;

	legacy_count = collide(0);
	if (legacy_count == -1)
		return 2;

	if (legacy_count == REFUSED)
		printf("DMABUF_IMPORT_ATTACH:     second import was refused (ESTALE);"
		       " its own is %d pages\n",
		       LARGE_PAGES);
	else
		printf("DMABUF_IMPORT_ATTACH:     second import was told %d pages, its own is %d%s\n",
		       legacy_count, LARGE_PAGES,
		       legacy_count == LARGE_PAGES ? "" : "  <- another buffer's");

	if (owned_count == REFUSED)
		printf("DMABUF_IMPORT_ATTACH_BDF: second import was refused (ESTALE);"
		       " its own is %d pages\n",
		       LARGE_PAGES);
	else
		printf("DMABUF_IMPORT_ATTACH_BDF: second import was told %d pages, its own is %d\n",
		       owned_count, LARGE_PAGES);

	if (owned_count != LARGE_PAGES) {
		fprintf(stderr, "FAIL: the bdf form did not give the second process"
				" its own buffer's pages\n");
		return 1;
	}

	/* Refusal and the right answer both pass; only the old silent handing
	 * over of the other process's buffer fails. */
	if (legacy_count != REFUSED && legacy_count != LARGE_PAGES) {
		fprintf(stderr, "FAIL: the old form gave the second process another"
				" buffer's pages\n");
		return 1;
	}

	return stale;
}
