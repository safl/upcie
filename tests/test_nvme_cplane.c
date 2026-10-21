// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * Hand a controller to another process and have it do I/O
 * =======================================================
 *
 * A server opens a controller and serves it over a socket. A client maps the
 * heap and BAR0 it is handed, reads LBA 0 on an allocated queue, and has the
 * server submit an identify into its buffer. Fails when the controller base,
 * the heap description or the queue layout does not survive the process
 * boundary. The
 * client is a separate process, not a fork, so it holds nothing it was not
 * handed. client-badversion checks that the server refuses another version.
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

	sock = socket(AF_UNIX, SOCK_SEQPACKET, 0);
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
	int sock = socket(AF_UNIX, SOCK_SEQPACKET, 0);

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
	size_t desc_off;
	int heap_fd;
	int hp_alive, iommufd_alive, ioas_alive, dmem_alive, heap_alive, ctrlr_alive;
	int export_alive;
};

static void
runtime_term(struct runtime *rt)
{
	if (rt->export_alive) {
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
 * Open the controller and leave a description of the heap in the heap
 *
 * Under vfio the controller is opened by its device file and never learns its
 * bdf, so it is written in by hand for the init reply to carry.
 */
static int
runtime_init(struct runtime *rt, const char *bdf, int use_vfio)
{
	struct hostmem_shared_desc *desc;
	size_t desc_nbytes;
	int err;

	rt->use_vfio = use_vfio;

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
	rt->export_alive = 1;

	desc = dmamem_heap_at_va(&rt->heap, rt->desc_off);

	err = use_vfio ? hostmem_shared_desc_fill_arithmetic(desc, rt->dmem.size,
							     rt->dmem.base_iova)
		       : hostmem_shared_desc_fill(desc, &rt->hp);
	if (err) {
		printf("# FAILED: hostmem_shared_desc_fill%s(); err(%d)\n",
		       use_vfio ? "_arithmetic" : "", err);
		return err;
	}

	return 0;
}

/**
 * The server: opens the controller, then answers until the client leaves.
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
			reply.result = -EPROTO;
			nvme_cplane_msg_send(sock, &reply, NULL, 0);
			continue;
		}

		switch (msg.op) {
		case NVME_CPLANE_OP_INIT_CONNECTION:
			reply.u.init_connection.ctrlr = rt.ctrlr.base;
			reply.u.init_connection.heap_nbytes = rt.dmem.size;
			reply.u.init_connection.desc_offset = rt.desc_off;
			reply.u.init_connection.bar0_nbytes = rt.ctrlr.func.bars[0].size;
			snprintf(reply.u.init_connection.bdf, sizeof(reply.u.init_connection.bdf),
				 "%s", rt.ctrlr.func.bdf);
			fds[nfds++] = rt.heap_fd;
			fds[nfds++] = rt.ctrlr.func.bars[0].fd;
			printf("server: attach, heap fd and description at 0x%" PRIx64 "\n",
			       reply.u.init_connection.desc_offset);
			break;

		case NVME_CPLANE_OP_STATUS:
			reply.u.status.ndevs = 1;
			reply.u.status.nclients = 1;
			reply.u.status.nclients_ctrlr = 1;
			reply.u.status.nqueues = granted_live ? 1 : 0;
			snprintf(reply.u.status.bdf, sizeof(reply.u.status.bdf), "%s",
				 rt.ctrlr.func.bdf);
			break;

		case NVME_CPLANE_OP_ALLOC_IOQPAIR:
			if (granted_live) {
				reply.result = -EBUSY;
				break;
			}
			reply.result = nvme_controller_create_io_qpair_dmamem(
				&rt.ctrlr, &allocated, msg.u.alloc_ioqpair.depth, &rt.heap,
				&sq_off, &cq_off, &prp_off);
			if (!reply.result) {
				granted_live = 1;
				reply.u.alloc_ioqpair.allocation.sq_offset = sq_off;
				reply.u.alloc_ioqpair.allocation.cq_offset = cq_off;
				reply.u.alloc_ioqpair.allocation.prp_offset = prp_off;
				reply.u.alloc_ioqpair.allocation.qid = allocated.qid;
				reply.u.alloc_ioqpair.allocation.depth = allocated.depth;
			}
			printf("server: allocation qid(%u) result(%d)\n", allocated.qid,
			       reply.result);
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

			reply.result =
				dmamem_heap_alloc_aligned(&rt.heap, msg.u.buf.nbytes, 4096, &off);
			reply.u.buf.offset = off;
			printf("server: buffer of %" PRIu64 " at 0x%zx result(%d)\n",
			       msg.u.buf.nbytes, off, reply.result);
			break;
		}

		case NVME_CPLANE_OP_FREE_BUF:
			dmamem_heap_free(&rt.heap, msg.u.buf.offset);
			break;

		case NVME_CPLANE_OP_ADMIN_CMD:
			reply.result = nvme_qpair_submit_sync(&rt.ctrlr.aq, &msg.u.admin_cmd.cmd,
							      rt.ctrlr.base.admin_timeout_ms,
							      &reply.u.admin_cmd.cpl);
			printf("server: admin opc(0x%x) result(%d)\n", msg.u.admin_cmd.cmd.opc,
			       reply.result);
			break;

		default:
			reply.result = -ENOSYS;
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
 * Map the heap and BAR0 and build a controller view from the init reply
 */
static int
view_init(struct view *v, int heap_fd, int bar0_fd, const struct nvme_cplane_init_connection *init)
{
	size_t heap_nbytes = init->heap_nbytes;
	size_t bar0_nbytes = init->bar0_nbytes;
	const struct hostmem_shared_desc *desc;
	int err;

	v->heap_nbytes = heap_nbytes;
	v->heap_base = mmap(NULL, heap_nbytes, PROT_READ | PROT_WRITE, MAP_SHARED, heap_fd, 0);
	if (v->heap_base == MAP_FAILED) {
		printf("# FAILED: mmap(heap); errno(%d)\n", errno);
		return -errno;
	}

	if (init->desc_offset >= heap_nbytes) {
		printf("# FAILED: description at 0x%" PRIx64 " is outside the heap\n",
		       init->desc_offset);
		return -ERANGE;
	}
	desc = (const struct hostmem_shared_desc *)((char *)v->heap_base + init->desc_offset);
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

	if (init->ctrlr.cap != nvme_mmio_cap_read(v->bar0)) {
		printf("# FAILED: the base's cap is not what this BAR0 reads\n");
		return -EINVAL;
	}

	memset(&v->ctrlr, 0, sizeof(v->ctrlr));
	v->ctrlr.base = init->ctrlr;
	v->ctrlr.func.bars[0].region = v->bar0;
	v->ctrlr.func.bars[0].size = bar0_nbytes;
	v->ctrlr.func.bars[0].fd = -1;
	snprintf(v->ctrlr.func.bdf, sizeof(v->ctrlr.func.bdf), "%.12s", init->bdf);

	return 0;
}

/**
 * A queue pair from an allocation: memory by offset, doorbells from this BAR0 mapping
 *
 * The PRP scratch must match nvme_request_pool_init_prps_dmamem(): one 4 KiB
 * page per request.
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
 *
 * With bad_version set, the first message is sent with nvme_cplane_msg_send()
 * since nvme_cplane_request() stamps the current version; passes only when
 * the server refuses it.
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
		msg.version = NVME_CPLANE_VERSION + 1;
		err = nvme_cplane_msg_send(sock, &msg, NULL, 0);
		if (!err) {
			err = nvme_cplane_msg_recv(sock, &msg, NULL, NULL);
		}
		if (err) {
			printf("# FAILED: version probe; err(%d)\n", err);
			return err;
		}
		if (msg.result != -EPROTO) {
			printf("# FAILED: server accepted version(%u), status(%d)\n",
			       NVME_CPLANE_VERSION + 1, msg.result);
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
		nvme_cplane_fds_close(fds, &nfds);
		return err ? err : -EPROTO;
	}
	printf("client: heap descriptor received, description at 0x%" PRIx64 "\n",
	       msg.u.init_connection.desc_offset);

	err = view_init(&view, fds[0], fds[1], &msg.u.init_connection);
	close(fds[0]);
	close(fds[1]);
	if (err) {
		goto exit;
	}
	printf("client: attached to %s\n", view.ctrlr.func.bdf);

	memset(&msg, 0, sizeof(msg));
	msg.op = NVME_CPLANE_OP_ALLOC_IOQPAIR;
	msg.u.alloc_ioqpair.depth = QUEUE_DEPTH;
	err = nvme_cplane_request(sock, &msg, NULL, NULL);
	if (err) {
		printf("# FAILED: allocation; err(%d)\n", err);
		goto exit;
	}
	printf("client: allocated qid(%u) depth(%u)\n", msg.u.alloc_ioqpair.allocation.qid,
	       msg.u.alloc_ioqpair.allocation.depth);

	err = view_qpair(&view, &msg.u.alloc_ioqpair.allocation, &qpair);
	if (err) {
		printf("# FAILED: building the queue pair; err(%d)\n", err);
		goto exit;
	}

	memset(&msg, 0, sizeof(msg));
	msg.op = NVME_CPLANE_OP_ALLOC_BUF;
	msg.u.buf.nbytes = PAYLOAD_NBYTES;
	err = nvme_cplane_request(sock, &msg, NULL, NULL);
	if (err) {
		printf("# FAILED: buffer; err(%d)\n", err);
		goto exit;
	}
	payload_off = msg.u.buf.offset;
	payload = (char *)view.heap_base + payload_off;

	cmd.opc = 0x2;
	cmd.nsid = 1;
	cmd.prp1 = dmamem_va_to_iova(&view.dmem, payload);
	{
		struct nvme_completion cpl = {0};

		err = nvme_qpair_submit_sync(&qpair, &cmd, view.ctrlr.base.timeout_ms, &cpl);
		if (err || cpl_status(&cpl)) {
			printf("# FAILED: read LBA 0; err(%d) status(0x%x)\n", err,
			       cpl_status(&cpl));
			err = err ? err : -EIO;
			goto exit;
		}
	}
	printf("# LGTM: read LBA 0 on an allocated queue\n");

	memset(payload, 0, PAYLOAD_NBYTES);
	memset(&msg, 0, sizeof(msg));
	msg.op = NVME_CPLANE_OP_ADMIN_CMD;
	msg.u.admin_cmd.cmd.opc = 0x6;
	msg.u.admin_cmd.cmd.nsid = 0;
	msg.u.admin_cmd.cmd.cdw10 = 0x1;
	msg.u.admin_cmd.cmd.prp1 = dmamem_va_to_iova(&view.dmem, payload);

	err = nvme_cplane_request(sock, &msg, NULL, NULL);
	if (err || cpl_status(&msg.u.admin_cmd.cpl)) {
		printf("# FAILED: identify; err(%d) status(0x%x)\n", err,
		       cpl_status(&msg.u.admin_cmd.cpl));
		err = err ? err : -EIO;
		goto exit;
	}

	{
		char sn[21] = {0};
		uint16_t vid;

		memcpy(&vid, payload, sizeof(vid));
		if (!vid) {
			printf("# FAILED: identify completed, but nothing landed here\n");
			err = -EIO;
			goto exit;
		}
		memcpy(sn, (char *)payload + 4, 20);
		printf("# LGTM: identify landed here, sn(%s)\n", sn);
	}

	memset(&msg, 0, sizeof(msg));
	msg.op = NVME_CPLANE_OP_FREE_BUF;
	msg.u.buf.offset = payload_off;
	nvme_cplane_request(sock, &msg, NULL, NULL);

	memset(&msg, 0, sizeof(msg));
	msg.op = NVME_CPLANE_OP_FREE_IOQPAIR;
	msg.u.free_ioqpair.qid = qpair.qid;
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
