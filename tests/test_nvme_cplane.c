// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * Hand a controller to another process and have it do I/O
 * =======================================================
 *
 * The first point at which any of this crosses a process boundary. An server
 * opens a controller and serves; a client connects, receives the heap
 * descriptor, finds the record through the heap header, asks for a queue, and
 * reads LBA 0 through it. Then it asks the server to submit an identify whose
 * payload lands in the client's own buffer, which is the part that would be
 * a copy if the payload travelled with the request.
 *
 * The client is a separate process rather than a fork, so that it holds
 * nothing it was not handed.
 *
 * The server refuses a peer speaking another version, which the
 * client-badversion mode checks, since a record whose layout comes from this
 * library cannot be read by something built against a different one.
 *
 * Usage:
 *   test_nvme_cplane server <bdf> <socket> [vfio]
 *   test_nvme_cplane client <socket>
 *   test_nvme_cplane client-badversion <socket>
 */
#define _UPCIE_WITH_NVME
#include <upcie/upcie.h>

#include <sys/un.h>

#define HEAP_NBYTES (64 * 1024 * 1024)
#define QUEUE_DEPTH 64

/**
 * Status with the phase tag masked off, since bit zero is not a status code
 */
static inline uint16_t
cpl_status(const struct nvme_completion *cpl)
{
	return cpl->status & 0xFFFE;
}

static int
listen_at(const char *path)
{
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	int sock;

	unlink(path);

	sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock < 0) {
		return -errno;
	}
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) || listen(sock, 1) ||
	    chmod(path, 0666)) {
		close(sock);
		return -errno;
	}

	return sock;
}

static int
connect_to(const char *path)
{
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	int sock = socket(AF_UNIX, SOCK_STREAM, 0);

	if (sock < 0) {
		return -errno;
	}
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

	if (connect(sock, (struct sockaddr *)&addr, sizeof(addr))) {
		close(sock);
		return -errno;
	}

	return sock;
}

/**
 * The server: opens the controller, then answers until the client leaves.
 *
 * The loop is here rather than in the library because the bookkeeping is the
 * server's: which client holds which queue, and what it is willing to submit.
 */
static int
server(const char *bdf, const char *path, int use_vfio)
{
	struct nvme_runtime_record *record;
	struct nvme_controller ctrlr = {0};
	struct vfio_ctx vfio = {0};
	struct hostmem_config config = {0};
	struct hostmem_heap heap = {0};
	struct nvme_qpair allocated = {0};
	struct nvme_cplane_msg msg;
	int listener, sock;
	int granted_live = 0;
	void *prps;
	int err;

	hostmem_config_init(&config);

	err = hostmem_heap_init(&heap, HEAP_NBYTES, &config);
	if (err) {
		printf("# FAILED: hostmem_heap_init(); err(%d)\n", err);
		return err;
	}

	/* Under vfio the device file cannot be bound twice, which is the whole
	 * reason a client is handed the descriptor rather than opening its own. */
	err = use_vfio ? nvme_controller_open_vfio(&ctrlr, &vfio, bdf, &heap)
		       : nvme_controller_open(&ctrlr, bdf, &heap);
	if (err) {
		printf("# FAILED: nvme_controller_open%s(); err(%d)\n", use_vfio ? "_vfio" : "",
		       err);
		return err;
	}

	record = hostmem_dma_malloc(&heap, sizeof(*record));
	if (!record || nvme_runtime_record_export(&ctrlr, heap.memory.size, record)) {
		printf("# FAILED: export the record\n");
		return -EIO;
	}
	hostmem_heap_record_set(&heap, (uint64_t)((char *)record - (char *)heap.memory.virt));

	prps = hostmem_dma_alloc_array(&heap, NVME_REQUEST_POOL_LEN, config.pagesize);
	if (!prps) {
		printf("# FAILED: hostmem_dma_alloc_array(prps)\n");
		return -ENOMEM;
	}

	listener = listen_at(path);
	if (listener < 0) {
		printf("# FAILED: listen_at(); err(%d)\n", listener);
		return listener;
	}
	printf("server: serving %s on %s\n", bdf, path);

	sock = accept(listener, NULL, NULL);
	if (sock < 0) {
		printf("# FAILED: accept(); errno(%d)\n", errno);
		return -errno;
	}

	{
		struct ucred cred = {0};

		if (nvme_cplane_peer_cred(sock, &cred)) {
			printf("# FAILED: nvme_cplane_peer_cred(); errno(%d)\n", errno);
			return -errno;
		}
		printf("server: serving pid(%d) uid(%u) gid(%u)\n", cred.pid, (unsigned)cred.uid,
		       (unsigned)cred.gid);
	}

	while (!(err = nvme_cplane_msg_recv(sock, &msg, NULL, NULL))) {
		struct nvme_cplane_msg reply = {.op = msg.op, .version = NVME_CPLANE_VERSION};
		int fds[NVME_CPLANE_FDS_MAX];
		uint32_t nfds = 0;

		if (msg.version != NVME_CPLANE_VERSION) {
			reply.status = -EPROTO;
			nvme_cplane_msg_send(sock, &reply, NULL, 0);
			continue;
		}

		switch (msg.op) {
		case NVME_CPLANE_OP_INIT_CONNECTION:
			reply.u.init.record_offset = hostmem_heap_record_get(&heap);
			reply.u.init.heap_nbytes = heap.memory.size;
			reply.u.init.bar0_nbytes = ctrlr.func.bars[0].size;
			fds[nfds++] = hostmem_heap_fd(&heap);
			fds[nfds++] = ctrlr.func.bars[0].fd;
			printf("server: attach, heap fd and record offset 0x%" PRIx64 "\n",
			       reply.u.init.record_offset);
			break;

		case NVME_CPLANE_OP_ALLOC_IOQPAIR:
			if (granted_live) {
				reply.status = -EBUSY;
				break;
			}
			reply.status = nvme_controller_create_io_qpair(&ctrlr, &allocated,
								       msg.u.queue.depth);
			if (!reply.status) {
				granted_live = 1;
				reply.status = nvme_ioqpair_export(&ctrlr, &allocated, prps,
								   &reply.u.queue.allocation);
			}
			printf("server: allocation qid(%u) status(%d)\n", allocated.qid,
			       reply.status);
			break;

		case NVME_CPLANE_OP_FREE_IOQPAIR:
			if (granted_live) {
				nvme_controller_delete_io_qpair(&ctrlr, &allocated);
				granted_live = 0;
			}
			printf("server: released\n");
			break;

		case NVME_CPLANE_OP_ADMIN_CMD:
			if (!nvme_cplane_admin_permitted(&msg.u.admin.cmd)) {
				reply.status = -EPERM;
				break;
			}
			reply.status = nvme_qpair_submit_sync(
				&ctrlr.aq, &msg.u.admin.cmd, ctrlr.timeout_ms, &reply.u.admin.cpl);
			printf("server: admin opc(0x%x) status(%d)\n", msg.u.admin.cmd.opc,
			       reply.status);
			break;

		default:
			reply.status = -ENOSYS;
			break;
		}

		reply.nfds = nfds;
		if (nvme_cplane_msg_send(sock, &reply, nfds ? fds : NULL, nfds)) {
			break;
		}
	}
	printf("server: client left (%d)\n", err);

	if (granted_live) {
		nvme_controller_delete_io_qpair(&ctrlr, &allocated);
	}
	close(sock);
	close(listener);
	unlink(path);
	if (use_vfio) {
		nvme_controller_close_vfio(&ctrlr, &vfio);
	} else {
		nvme_controller_close(&ctrlr);
	}
	hostmem_heap_term(&heap);

	return 0;
}

/**
 * The client: holds nothing it was not handed.
 */
static int
client(const char *path, int bad_version)
{
	struct nvme_controller ctrlr = {0};
	struct hostmem_config config = {0};
	struct hostmem_heap heap = {0};
	struct nvme_qpair qpair = {0};
	struct nvme_cplane_msg msg = {0};
	struct nvme_command cmd = {0};
	int fds[NVME_CPLANE_FDS_MAX];
	uint32_t nfds = 0;
	void *payload;
	void *bar0;
	int sock, err;

	hostmem_config_init(&config);

	sock = connect_to(path);
	if (sock < 0) {
		printf("# FAILED: connect_to(); err(%d)\n", sock);
		return sock;
	}

	msg.op = NVME_CPLANE_OP_INIT_CONNECTION;
	if (bad_version) {
		/* nvme_cplane_request() stamps the current version, so the
		 * mismatch has to be sent by hand. */
		msg.version = NVME_CPLANE_VERSION + 1;
		err = nvme_cplane_msg_send(sock, &msg, NULL, 0);
		if (!err) {
			err = nvme_cplane_msg_recv(sock, &msg, NULL, NULL);
		}
		if (err) {
			printf("# FAILED: version probe; err(%d)\n", err);
			return err;
		}
		if (msg.status != -EPROTO) {
			printf("# FAILED: server accepted version(%u), status(%d)\n",
			       NVME_CPLANE_VERSION + 1, msg.status);
			return -EIO;
		}
		printf("# LGTM: server refused version(%u) with EPROTO\n",
		       NVME_CPLANE_VERSION + 1);
		close(sock);
		return 0;
	}

	err = nvme_cplane_request(sock, &msg, fds, &nfds);
	if (err || (nfds != 2)) {
		printf("# FAILED: attach; err(%d) nfds(%u)\n", err, nfds);
		return err ? err : -EPROTO;
	}
	printf("client: heap descriptor received, record at 0x%" PRIx64 "\n",
	       msg.u.init.record_offset);

	err = hostmem_heap_attach(&heap, fds[0], &config);
	close(fds[0]);
	if (err) {
		printf("# FAILED: hostmem_heap_attach(); err(%d)\n", err);
		return err;
	}

	bar0 = mmap(NULL, msg.u.init.bar0_nbytes, PROT_READ | PROT_WRITE, MAP_SHARED, fds[1], 0);
	close(fds[1]);
	if (bar0 == MAP_FAILED) {
		printf("# FAILED: mmap(BAR0); errno(%d)\n", errno);
		return -errno;
	}

	err = nvme_runtime_record_import(
		&ctrlr,
		(const struct nvme_runtime_record *)((char *)heap.memory.virt +
						     hostmem_heap_record_get(&heap)),
		bar0, &heap);
	if (err) {
		printf("# FAILED: nvme_runtime_record_import(); err(%d)\n", err);
		return err;
	}
	printf("client: attached to %s\n", ctrlr.func.bdf);

	memset(&msg, 0, sizeof(msg));
	msg.op = NVME_CPLANE_OP_ALLOC_IOQPAIR;
	msg.u.queue.depth = QUEUE_DEPTH;
	err = nvme_cplane_request(sock, &msg, NULL, NULL);
	if (err) {
		printf("# FAILED: allocation; err(%d)\n", err);
		return err;
	}
	printf("client: allocated qid(%u) depth(%u)\n", msg.u.queue.allocation.qid,
	       msg.u.queue.allocation.depth);

	err = nvme_ioqpair_import(&qpair, &msg.u.queue.allocation, &ctrlr);
	if (err) {
		printf("# FAILED: nvme_ioqpair_import(); err(%d)\n", err);
		return err;
	}

	payload = (char *)heap.memory.virt + msg.u.queue.allocation.prp_offset;

	cmd.opc = 0x2; ///< Read
	cmd.nsid = 1;
	cmd.prp1 = hostmem_dma_v2p(&heap, payload);
	{
		struct nvme_completion cpl = {0};

		err = nvme_qpair_submit_sync(&qpair, &cmd, ctrlr.timeout_ms, &cpl);
		if (err || cpl_status(&cpl)) {
			printf("# FAILED: read LBA 0; err(%d) status(0x%x)\n", err,
			       cpl_status(&cpl));
			return err ? err : -EIO;
		}
	}
	printf("# LGTM: read LBA 0 on a allocated queue\n");

	/* And an admin command, whose payload lands here rather than travelling
	 * back through the socket. */
	memset(payload, 0, 4096);
	memset(&msg, 0, sizeof(msg));
	msg.op = NVME_CPLANE_OP_ADMIN_CMD;
	msg.u.admin.cmd.opc = 0x6; ///< Identify
	msg.u.admin.cmd.nsid = 0;
	msg.u.admin.cmd.cdw10 = 0x1; ///< Identify Controller
	msg.u.admin.cmd.prp1 = hostmem_dma_v2p(&heap, payload);

	err = nvme_cplane_request(sock, &msg, NULL, NULL);
	if (err || cpl_status(&msg.u.admin.cpl)) {
		printf("# FAILED: identify; err(%d) status(0x%x)\n", err,
		       cpl_status(&msg.u.admin.cpl));
		return err ? err : -EIO;
	}

	{
		char sn[21] = {0};

		memcpy(sn, (char *)payload + 4, 20);
		printf("# LGTM: identify landed here, sn(%s)\n", sn);
	}

	memset(&msg, 0, sizeof(msg));
	msg.op = NVME_CPLANE_OP_FREE_IOQPAIR;
	msg.u.release.qid = qpair.qid;
	nvme_cplane_request(sock, &msg, NULL, NULL);

	nvme_ioqpair_release(&qpair);
	hostmem_heap_detach(&heap);
	close(sock);

	return 0;
}

int
main(int argc, char *argv[])
{
	setvbuf(stdout, NULL, _IOLBF, 0);

	if ((argc == 4) && !strcmp(argv[1], "server")) {
		return server(argv[2], argv[3], 0) ? EXIT_FAILURE : EXIT_SUCCESS;
	}
	if ((argc == 5) && !strcmp(argv[1], "server") && !strcmp(argv[4], "vfio")) {
		return server(argv[2], argv[3], 1) ? EXIT_FAILURE : EXIT_SUCCESS;
	}
	if ((argc == 3) && !strcmp(argv[1], "client")) {
		return client(argv[2], 0) ? EXIT_FAILURE : EXIT_SUCCESS;
	}
	if ((argc == 3) && !strcmp(argv[1], "client-badversion")) {
		return client(argv[2], 1) ? EXIT_FAILURE : EXIT_SUCCESS;
	}

	fprintf(stderr, "usage: %s server <bdf> <socket> [vfio]\n", argv[0]);
	fprintf(stderr, "       %s client <socket>\n", argv[0]);
	fprintf(stderr, "       %s client-badversion <socket>\n", argv[0]);

	return 2;
}
