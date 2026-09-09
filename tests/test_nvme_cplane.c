// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * Hand a controller to another process and have it do I/O
 * =======================================================
 *
 * The first point at which any of this crosses a process boundary. A server
 * opens a controller and serves; a client connects, receives the heap and BAR0
 * descriptors, finds the record and the heap's description at the offsets it
 * was told, asks for a queue and a buffer, and reads LBA 0 through it. Then it
 * asks the server to submit an identify whose payload lands in the client's
 * buffer, which is the part that would be a copy if the payload travelled with
 * the request.
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

#include <linux/limits.h>
#include <sys/un.h>

#define HEAP_NBYTES (64ULL * 1024 * 1024)
#define HUGEPGSZ (2ULL * 1024 * 1024)
#define QUEUE_DEPTH 64
#define PAYLOAD_NBYTES 4096

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

static int
resolve_vfio_cdev(const char *bdf, char *cdev_path, size_t cdev_path_len)
{
	char sysfs_path[PATH_MAX] = {0};
	DIR *dir;
	struct dirent *ent;
	int found = 0;

	snprintf(sysfs_path, sizeof(sysfs_path), "/sys/bus/pci/devices/%s/vfio-dev", bdf);
	dir = opendir(sysfs_path);
	if (!dir) {
		return -errno;
	}
	while ((ent = readdir(dir))) {
		if (strncmp(ent->d_name, "vfio", 4) != 0) {
			continue;
		}
		snprintf(cdev_path, cdev_path_len, "/dev/vfio/devices/%s", ent->d_name);
		found = 1;
		break;
	}
	closedir(dir);
	return found ? 0 : -ENOENT;
}

/**
 * What the server holds: the heap, the controller and what it exported
 *
 * Under uio_pci_generic the heap is a hugepage the controller reaches by
 * physical address; under vfio-pci it is a memfd mapped into the IOAS the
 * controller is attached to. Both are a dmamem, and everything from the heap
 * onwards is the same code.
 */
struct runtime {
	int use_vfio;
	struct hostmem_config config;
	struct hostmem_hugepage hp;
	struct iommufd iommufd;
	struct dmamem dmem;
	struct dmamem_heap heap;
	struct nvme_controller ctrlr;
	union {
		struct nvme_dmamem_uio_ctx uio;
		struct nvme_dmamem_vfio_ctx vfio;
	} attach;
	size_t desc_off, record_off;
	int heap_fd;
	int hp_alive, iommufd_alive, ioas_alive, dmem_alive, heap_alive, ctrlr_alive;
	int export_alive;
};

static void
runtime_term(struct runtime *rt)
{
	if (rt->export_alive) {
		dmamem_heap_free(&rt->heap, rt->record_off);
		dmamem_heap_free(&rt->heap, rt->desc_off);
	}
	if (rt->ctrlr_alive) {
		if (rt->use_vfio) {
			nvme_controller_close_dmamem_vfio(&rt->ctrlr, &rt->attach.vfio, &rt->heap);
		} else {
			nvme_controller_close_dmamem_uio(&rt->ctrlr, &rt->attach.uio, &rt->heap);
		}
	}
	if (rt->heap_alive) {
		dmamem_heap_term(&rt->heap);
	}
	if (rt->dmem_alive) {
		dmamem_destroy(&rt->dmem);
	}
	if (rt->hp_alive) {
		hostmem_hugepage_free(&rt->hp);
	}
	if (rt->ioas_alive) {
		iommufd_destroy(&rt->iommufd, rt->iommufd.ioas_id);
	}
	if (rt->iommufd_alive) {
		iommufd_close(&rt->iommufd);
	}
}

/**
 * Open the controller and leave a record and a description of the heap in the heap
 */
static int
runtime_init(struct runtime *rt, const char *bdf, int use_vfio)
{
	struct hostmem_shared_desc *desc;
	struct nvme_runtime_record *record;
	size_t desc_nbytes;
	int err;

	rt->use_vfio = use_vfio;

	/* Under vfio the device file cannot be bound twice, which is the whole
	 * reason a client is handed the descriptor rather than opening its own. */
	if (use_vfio) {
		char cdev[PATH_MAX] = {0};

		err = resolve_vfio_cdev(bdf, cdev, sizeof(cdev));
		if (err) {
			printf("# FAILED: resolve_vfio_cdev(%s); err(%d)\n", bdf, err);
			return err;
		}
		if (iommufd_open(&rt->iommufd)) {
			printf("# FAILED: iommufd_open(); errno(%d)\n", errno);
			return -errno;
		}
		rt->iommufd_alive = 1;
		if (iommufd_ioas_alloc(&rt->iommufd)) {
			printf("# FAILED: iommufd_ioas_alloc(); errno(%d)\n", errno);
			return -errno;
		}
		rt->ioas_alive = 1;

		err = dmamem_from_memfd(&rt->dmem, &rt->iommufd, HEAP_NBYTES, HUGEPGSZ);
		if (err) {
			printf("# FAILED: dmamem_from_memfd(); err(%d)\n", err);
			return err;
		}
		rt->dmem_alive = 1;

		err = dmamem_heap_init(&rt->heap, &rt->dmem, 4096);
		if (err) {
			printf("# FAILED: dmamem_heap_init(); err(%d)\n", err);
			return err;
		}
		rt->heap_alive = 1;

		nvme_dmamem_vfio_ctx_init(&rt->attach.vfio);
		err = nvme_controller_open_dmamem_vfio(&rt->ctrlr, &rt->attach.vfio, &rt->iommufd,
						       &rt->heap, cdev);
		if (err) {
			printf("# FAILED: nvme_controller_open_dmamem_vfio(%s); err(%d)\n", cdev,
			       err);
			return err;
		}
		rt->ctrlr_alive = 1;
		rt->heap_fd = rt->dmem.fd;
		/* Opened by its vfio device file, so the controller does not know
		 * its own address; the record should name it all the same. */
		snprintf(rt->ctrlr.func.bdf, sizeof(rt->ctrlr.func.bdf), "%.12s", bdf);
	} else {
		if (hostmem_config_init(&rt->config) ||
		    hostmem_hugepage_alloc(HEAP_NBYTES, &rt->hp, &rt->config)) {
			printf("# FAILED: hostmem_hugepage_alloc(); are hugepages reserved?\n");
			return -ENOMEM;
		}
		rt->hp_alive = 1;

		err = dmamem_from_hostmem_registry(&rt->dmem, &rt->hp, 0);
		if (err) {
			printf("# FAILED: dmamem_from_hostmem_registry(); err(%d)\n", err);
			return err;
		}
		rt->dmem_alive = 1;

		err = dmamem_heap_init(&rt->heap, &rt->dmem, 4096);
		if (err) {
			printf("# FAILED: dmamem_heap_init(); err(%d)\n", err);
			return err;
		}
		rt->heap_alive = 1;

		nvme_dmamem_uio_ctx_init(&rt->attach.uio);
		err = nvme_controller_open_dmamem_uio(&rt->ctrlr, &rt->attach.uio, &rt->heap, bdf);
		if (err) {
			printf("# FAILED: nvme_controller_open_dmamem_uio(%s); err(%d)\n", bdf,
			       err);
			return err;
		}
		rt->ctrlr_alive = 1;
		rt->heap_fd = rt->hp.fd;
	}

	desc_nbytes = use_vfio ? sizeof(*desc) : hostmem_shared_desc_nbytes(rt->hp.nphys);
	err = dmamem_heap_alloc(&rt->heap, desc_nbytes, &rt->desc_off);
	if (err) {
		printf("# FAILED: dmamem_heap_alloc(desc); err(%d)\n", err);
		return err;
	}
	err = dmamem_heap_alloc(&rt->heap, sizeof(*record), &rt->record_off);
	if (err) {
		dmamem_heap_free(&rt->heap, rt->desc_off);
		printf("# FAILED: dmamem_heap_alloc(record); err(%d)\n", err);
		return err;
	}
	rt->export_alive = 1;

	desc = dmamem_heap_at_va(&rt->heap, rt->desc_off);
	record = dmamem_heap_at_va(&rt->heap, rt->record_off);

	err = use_vfio ? hostmem_shared_desc_fill_arithmetic(desc, rt->dmem.size,
							     rt->dmem.base_iova)
		       : hostmem_shared_desc_fill(desc, &rt->hp);
	if (err) {
		printf("# FAILED: hostmem_shared_desc_fill%s(); err(%d)\n",
		       use_vfio ? "_arithmetic" : "", err);
		return err;
	}

	err = nvme_runtime_record_export(&rt->ctrlr, rt->dmem.size, record);
	if (err) {
		printf("# FAILED: nvme_runtime_record_export(); err(%d)\n", err);
		return err;
	}
	record->desc_offset = rt->desc_off;

	return 0;
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
	struct runtime rt = {0};
	struct nvme_qpair allocated = {0};
	struct nvme_cplane_msg msg;
	size_t sq_off = 0, cq_off = 0, prp_off = 0;
	int listener, sock;
	int granted_live = 0;
	int err;

	err = runtime_init(&rt, bdf, use_vfio);
	if (err) {
		runtime_term(&rt);
		return err;
	}

	listener = listen_at(path);
	if (listener < 0) {
		printf("# FAILED: listen_at(); err(%d)\n", listener);
		runtime_term(&rt);
		return listener;
	}
	printf("server: serving %s on %s\n", bdf, path);

	sock = accept(listener, NULL, NULL);
	if (sock < 0) {
		printf("# FAILED: accept(); errno(%d)\n", errno);
		runtime_term(&rt);
		return -errno;
	}

	{
		struct ucred cred = {0};

		if (nvme_cplane_peer_cred(sock, &cred)) {
			printf("# FAILED: nvme_cplane_peer_cred(); errno(%d)\n", errno);
			runtime_term(&rt);
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
			reply.u.init.record_offset = rt.record_off;
			reply.u.init.heap_nbytes = rt.dmem.size;
			reply.u.init.bar0_nbytes = rt.ctrlr.func.bars[0].size;
			fds[nfds++] = rt.heap_fd;
			fds[nfds++] = rt.ctrlr.func.bars[0].fd;
			printf("server: attach, heap fd and record offset 0x%" PRIx64 "\n",
			       reply.u.init.record_offset);
			break;

		case NVME_CPLANE_OP_ALLOC_IOQPAIR:
			if (granted_live) {
				reply.status = -EBUSY;
				break;
			}
			reply.status = nvme_controller_create_io_qpair_dmamem(
				&rt.ctrlr, &allocated, msg.u.queue.depth, &rt.heap, &sq_off,
				&cq_off, &prp_off);
			if (!reply.status) {
				granted_live = 1;
				reply.u.queue.allocation.sq_offset = sq_off;
				reply.u.queue.allocation.cq_offset = cq_off;
				reply.u.queue.allocation.prp_offset = prp_off;
				reply.u.queue.allocation.qid = allocated.qid;
				reply.u.queue.allocation.depth = allocated.depth;
			}
			printf("server: allocation qid(%u) status(%d)\n", allocated.qid,
			       reply.status);
			break;

		case NVME_CPLANE_OP_FREE_IOQPAIR:
			if (granted_live) {
				nvme_controller_delete_io_qpair_dmamem(
					&rt.ctrlr, &allocated, &rt.heap, sq_off, cq_off, prp_off);
				granted_live = 0;
			}
			printf("server: released\n");
			break;

		case NVME_CPLANE_OP_ALLOC_BUF: {
			size_t off = 0;

			reply.status =
				dmamem_heap_alloc_aligned(&rt.heap, msg.u.mem.nbytes, 4096, &off);
			reply.u.mem.offset = off;
			printf("server: buffer of %" PRIu64 " at 0x%zx status(%d)\n",
			       msg.u.mem.nbytes, off, reply.status);
			break;
		}

		case NVME_CPLANE_OP_FREE_BUF:
			dmamem_heap_free(&rt.heap, msg.u.mem.offset);
			break;

		case NVME_CPLANE_OP_ADMIN_CMD:
			if (!nvme_cplane_admin_permitted(&msg.u.admin.cmd)) {
				reply.status = -EPERM;
				break;
			}
			reply.status =
				nvme_qpair_submit_sync(&rt.ctrlr.aq, &msg.u.admin.cmd,
						       rt.ctrlr.timeout_ms, &reply.u.admin.cpl);
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
		nvme_controller_delete_io_qpair_dmamem(&rt.ctrlr, &allocated, &rt.heap, sq_off,
						       cq_off, prp_off);
	}
	close(sock);
	close(listener);
	unlink(path);
	runtime_term(&rt);

	return 0;
}

/**
 * What a client holds: its own mappings and what it built from them
 */
struct view {
	void *heap_base;
	size_t heap_nbytes;
	void *bar0;
	size_t bar0_nbytes;
	struct dmamem dmem;
	struct nvme_controller ctrlr;
	int dmem_alive;
};

static void
view_term(struct view *v)
{
	if (v->dmem_alive) {
		dmamem_destroy(&v->dmem);
	}
	if (v->bar0 && (v->bar0 != MAP_FAILED)) {
		munmap(v->bar0, v->bar0_nbytes);
	}
	if (v->heap_base && (v->heap_base != MAP_FAILED)) {
		munmap(v->heap_base, v->heap_nbytes);
	}
}

/**
 * Everything a client does with what it is handed: two descriptors and an offset
 */
static int
view_init(struct view *v, int heap_fd, size_t heap_nbytes, int bar0_fd, size_t bar0_nbytes,
	  uint64_t record_offset)
{
	const struct nvme_runtime_record *record;
	const struct hostmem_shared_desc *desc;
	int err;

	v->heap_nbytes = heap_nbytes;
	v->heap_base = mmap(NULL, heap_nbytes, PROT_READ | PROT_WRITE, MAP_SHARED, heap_fd, 0);
	if (v->heap_base == MAP_FAILED) {
		printf("# FAILED: mmap(heap); errno(%d)\n", errno);
		return -errno;
	}

	record = (const struct nvme_runtime_record *)((char *)v->heap_base + record_offset);
	if (record->version != NVME_RUNTIME_RECORD_VERSION) {
		printf("# FAILED: record version(%u), expected(%u)\n", record->version,
		       NVME_RUNTIME_RECORD_VERSION);
		return -EPROTO;
	}
	if (record->heap_nbytes != heap_nbytes) {
		printf("# FAILED: record says the heap is %" PRIu64 " bytes, mapped %zu\n",
		       record->heap_nbytes, heap_nbytes);
		return -EINVAL;
	}

	desc = (const struct hostmem_shared_desc *)((char *)v->heap_base + record->desc_offset);
	err = dmamem_from_shared_hostmem(&v->dmem, v->heap_base, desc, 0);
	if (err) {
		printf("# FAILED: dmamem_from_shared_hostmem(); err(%d)\n", err);
		return err;
	}
	v->dmem_alive = 1;

	v->bar0_nbytes = bar0_nbytes;
	v->bar0 = mmap(NULL, bar0_nbytes, PROT_READ | PROT_WRITE, MAP_SHARED, bar0_fd, 0);
	if (v->bar0 == MAP_FAILED) {
		printf("# FAILED: mmap(BAR0); errno(%d)\n", errno);
		return -errno;
	}

	memset(&v->ctrlr, 0, sizeof(v->ctrlr));
	v->ctrlr.timeout_ms = (int)record->timeout_ms;
	v->ctrlr.cc = record->cc;
	v->ctrlr.func.bars[0].region = v->bar0;
	v->ctrlr.func.bars[0].size = bar0_nbytes;
	v->ctrlr.func.bars[0].fd = -1;
	snprintf(v->ctrlr.func.bdf, sizeof(v->ctrlr.func.bdf), "%.12s", record->bdf);

	return 0;
}

/**
 * A queue pair from an allocation: memory by offset, doorbells from this BAR0 mapping
 */
static int
view_qpair(struct view *v, const struct nvme_ioqpair *allocation, struct nvme_qpair *qpair)
{
	char *base = v->heap_base;
	int dstrd = nvme_reg_cap_get_dstrd(nvme_mmio_cap_read(v->bar0));

	if (!allocation->qid || (allocation->sq_offset >= v->heap_nbytes) ||
	    (allocation->cq_offset >= v->heap_nbytes) ||
	    (allocation->prp_offset >= v->heap_nbytes)) {
		return -ERANGE;
	}

	memset(qpair, 0, sizeof(*qpair));
	qpair->qid = allocation->qid;
	qpair->depth = allocation->depth;
	qpair->sq = base + allocation->sq_offset;
	qpair->cq = base + allocation->cq_offset;
	qpair->sqdb = (char *)v->bar0 + 0x1000 + ((2 * allocation->qid) << (2 + dstrd));
	qpair->cqdb = (char *)v->bar0 + 0x1000 + ((2 * allocation->qid + 1) << (2 + dstrd));
	qpair->tail_last_written = UINT16_MAX;
	qpair->phase = 1;

	qpair->rpool = calloc(1, sizeof(*qpair->rpool));
	if (!qpair->rpool) {
		return -errno;
	}
	nvme_request_pool_init(qpair->rpool);

	/* The scratch was laid out by nvme_request_pool_init_prps_dmamem(), one
	 * 4 KiB page per request. */
	qpair->rpool->prps = base + allocation->prp_offset;
	for (uint16_t i = 0; i < NVME_REQUEST_POOL_LEN; ++i) {
		void *prp = base + allocation->prp_offset + ((size_t)i * 4096);

		qpair->rpool->reqs[i].prp = prp;
		qpair->rpool->reqs[i].prp_addr = dmamem_va_to_iova(&v->dmem, prp);
	}

	return 0;
}

static void
view_qpair_release(struct nvme_qpair *qpair)
{
	free(qpair->rpool);
	memset(qpair, 0, sizeof(*qpair));
}

/**
 * The client: holds nothing it was not handed.
 */
static int
client(const char *path, int bad_version)
{
	struct view view = {0};
	struct nvme_qpair qpair = {0};
	struct nvme_cplane_msg msg = {0};
	struct nvme_command cmd = {0};
	int fds[NVME_CPLANE_FDS_MAX];
	uint32_t nfds = 0;
	uint64_t payload_off = 0;
	void *payload;
	int sock, err;

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

	err = view_init(&view, fds[0], msg.u.init.heap_nbytes, fds[1], msg.u.init.bar0_nbytes,
			msg.u.init.record_offset);
	close(fds[0]);
	close(fds[1]);
	if (err) {
		goto exit;
	}
	printf("client: attached to %s\n", view.ctrlr.func.bdf);

	memset(&msg, 0, sizeof(msg));
	msg.op = NVME_CPLANE_OP_ALLOC_IOQPAIR;
	msg.u.queue.depth = QUEUE_DEPTH;
	err = nvme_cplane_request(sock, &msg, NULL, NULL);
	if (err) {
		printf("# FAILED: allocation; err(%d)\n", err);
		goto exit;
	}
	printf("client: allocated qid(%u) depth(%u)\n", msg.u.queue.allocation.qid,
	       msg.u.queue.allocation.depth);

	err = view_qpair(&view, &msg.u.queue.allocation, &qpair);
	if (err) {
		printf("# FAILED: building the queue pair; err(%d)\n", err);
		goto exit;
	}

	/* A client cannot allocate from the heap, since the allocator is the
	 * server's; it asks, and is answered with an offset. */
	memset(&msg, 0, sizeof(msg));
	msg.op = NVME_CPLANE_OP_ALLOC_BUF;
	msg.u.mem.nbytes = PAYLOAD_NBYTES;
	err = nvme_cplane_request(sock, &msg, NULL, NULL);
	if (err) {
		printf("# FAILED: buffer; err(%d)\n", err);
		goto exit;
	}
	payload_off = msg.u.mem.offset;
	payload = (char *)view.heap_base + payload_off;

	cmd.opc = 0x2; ///< Read
	cmd.nsid = 1;
	cmd.prp1 = dmamem_va_to_iova(&view.dmem, payload);
	{
		struct nvme_completion cpl = {0};

		err = nvme_qpair_submit_sync(&qpair, &cmd, view.ctrlr.timeout_ms, &cpl);
		if (err || cpl_status(&cpl)) {
			printf("# FAILED: read LBA 0; err(%d) status(0x%x)\n", err,
			       cpl_status(&cpl));
			err = err ? err : -EIO;
			goto exit;
		}
	}
	printf("# LGTM: read LBA 0 on an allocated queue\n");

	/* And an admin command, whose payload lands here rather than travelling
	 * back through the socket. */
	memset(payload, 0, PAYLOAD_NBYTES);
	memset(&msg, 0, sizeof(msg));
	msg.op = NVME_CPLANE_OP_ADMIN_CMD;
	msg.u.admin.cmd.opc = 0x6; ///< Identify
	msg.u.admin.cmd.nsid = 0;
	msg.u.admin.cmd.cdw10 = 0x1; ///< Identify Controller
	msg.u.admin.cmd.prp1 = dmamem_va_to_iova(&view.dmem, payload);

	err = nvme_cplane_request(sock, &msg, NULL, NULL);
	if (err || cpl_status(&msg.u.admin.cpl)) {
		printf("# FAILED: identify; err(%d) status(0x%x)\n", err,
		       cpl_status(&msg.u.admin.cpl));
		err = err ? err : -EIO;
		goto exit;
	}

	{
		char sn[21] = {0};

		memcpy(sn, (char *)payload + 4, 20);
		printf("# LGTM: identify landed here, sn(%s)\n", sn);
	}

	memset(&msg, 0, sizeof(msg));
	msg.op = NVME_CPLANE_OP_FREE_BUF;
	msg.u.mem.offset = payload_off;
	nvme_cplane_request(sock, &msg, NULL, NULL);

	memset(&msg, 0, sizeof(msg));
	msg.op = NVME_CPLANE_OP_FREE_IOQPAIR;
	msg.u.release.qid = qpair.qid;
	nvme_cplane_request(sock, &msg, NULL, NULL);

exit:
	if (qpair.rpool) {
		view_qpair_release(&qpair);
	}
	view_term(&view);
	close(sock);

	return err;
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
