// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * Rebuild a controller from its base, in one process
 * ==================================================
 *
 * Rebuild a controller and queue pair from the controller's base and a queue
 * description over a second mapping of the heap, and read LBA 0 through it. Same process,
 * no sockets, so a fault here is in the reconstruction, not the control plane.
 * Fails when the rebuilt queue rings another doorbell (compared as BAR0
 * offsets) or resolves to other device addresses. The second mapping takes its
 * addresses from the description, not pagemap, as an unprivileged client must.
 *
 * Usage:
 *   test_nvme_controller_rebuild <bdf> [vfio]
 *   e.g.
 *   test_nvme_controller_rebuild 0000:c4:00.0
 *   test_nvme_controller_rebuild 0000:c4:00.0 vfio
 */
#define _UPCIE_WITH_NVME
#include <upcie/upcie.h>

#include <linux/limits.h>

#define HEAP_NBYTES (64ULL * 1024 * 1024)
#define HUGEPGSZ (2ULL * 1024 * 1024)
#define NBYTES 4096

/**
 * What the serving side holds: the heap, the controller and what it exported
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
 * Map the heap and BAR0 and build a controller view around the base
 */
static int
view_init(struct view *v, int heap_fd, size_t heap_nbytes, int bar0_fd, size_t bar0_nbytes,
	  const struct nvme_controller_base *base, uint64_t desc_offset)
{
	const struct hostmem_shared_desc *desc;
	int err;

	v->heap_nbytes = heap_nbytes;
	v->heap_base = mmap(NULL, heap_nbytes, PROT_READ | PROT_WRITE, MAP_SHARED, heap_fd, 0);
	if (v->heap_base == MAP_FAILED) {
		printf("# FAILED: mmap(heap); errno(%d)\n", errno);
		return -errno;
	}

	desc = (const struct hostmem_shared_desc *)((char *)v->heap_base + desc_offset);
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

	/* The base is what the controller was opened with; this BAR0 mapping
	 * must show the same controller, and enabled the way the base says. */
	if (base->cap != nvme_mmio_cap_read(v->bar0)) {
		printf("# FAILED: base cap(0x%" PRIx64 ") is not what BAR0 reads\n", base->cap);
		return -EINVAL;
	}
	if (!nvme_reg_cc_get_en(base->cc) || !base->timeout_ms || !base->admin_timeout_ms) {
		printf("# FAILED: base cc(0x%x) timeout_ms(%u) admin_timeout_ms(%u)\n", base->cc,
		       base->timeout_ms, base->admin_timeout_ms);
		return -EINVAL;
	}

	memset(&v->ctrlr, 0, sizeof(v->ctrlr));
	v->ctrlr.base = *base;
	v->ctrlr.func.bars[0].region = v->bar0;
	v->ctrlr.func.bars[0].size = bar0_nbytes;
	v->ctrlr.func.bars[0].fd = -1;

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

int
main(int argc, char *argv[])
{
	struct runtime rt = {0};
	struct view view = {0};
	struct nvme_ioqpair allocation = {0};
	struct nvme_qpair qpair = {0};
	struct nvme_qpair rebuilt = {0};
	struct nvme_command cmd = {0};
	struct nvme_completion cpl = {0};
	size_t sq_off = 0, cq_off = 0, prp_off = 0, payload_off = 0;
	void *payload;
	int qpair_alive = 0, payload_alive = 0;
	int err;

	if ((argc < 2) || (argc > 3) || ((argc == 3) && strcmp(argv[2], "vfio"))) {
		fprintf(stderr, "usage: %s <bdf> [vfio]\n", argv[0]);
		return 2;
	}

	err = runtime_init(&rt, argv[1], argc == 3);
	if (err) {
		goto exit;
	}

	err = nvme_controller_create_io_qpair_dmamem(&rt.ctrlr, &qpair, 64, &rt.heap, &sq_off,
						     &cq_off, &prp_off);
	if (err) {
		printf("# FAILED: nvme_controller_create_io_qpair_dmamem(); err(%d)\n", err);
		goto exit;
	}
	qpair_alive = 1;

	allocation.sq_offset = sq_off;
	allocation.cq_offset = cq_off;
	allocation.prp_offset = prp_off;
	allocation.qid = qpair.qid;
	allocation.depth = qpair.depth;

	printf("base: cap(0x%" PRIx64 ") cc(0x%x) timeout_ms(%u)\n", rt.ctrlr.base.cap,
	       rt.ctrlr.base.cc, rt.ctrlr.base.timeout_ms);
	printf("allocation: qid(%u) depth(%u) sq_offset(0x%" PRIx64 ") cq_offset(0x%" PRIx64 ")\n",
	       allocation.qid, allocation.depth, allocation.sq_offset, allocation.cq_offset);

	err = view_init(&view, rt.heap_fd, rt.dmem.size, rt.ctrlr.func.bars[0].fd,
			rt.ctrlr.func.bars[0].size, &rt.ctrlr.base, rt.desc_off);
	if (err) {
		goto exit;
	}
	printf("# LGTM: mapped by descriptor, described as %s\n",
	       rt.use_vfio ? "one base address" : "a table of physical addresses");

	err = view_qpair(&view, &allocation, &rebuilt);
	if (err) {
		printf("# FAILED: rebuilding the queue pair; err(%d)\n", err);
		goto exit;
	}

	if (((char *)rebuilt.sqdb - (char *)view.bar0) !=
		    ((char *)qpair.sqdb - (char *)rt.ctrlr.func.bars[0].region) ||
	    ((char *)rebuilt.cqdb - (char *)view.bar0) !=
		    ((char *)qpair.cqdb - (char *)rt.ctrlr.func.bars[0].region)) {
		printf("# FAILED: rebuilt queue rings a different doorbell\n");
		err = -EIO;
		goto exit;
	}
	if (dmamem_va_to_iova(&view.dmem, rebuilt.sq) != dmamem_heap_at_iova(&rt.heap, sq_off)) {
		printf("# FAILED: rebuilt queue resolves to different memory\n");
		err = -EIO;
		goto exit;
	}
	printf("# LGTM: rebuilt queue resolves to the same doorbell and memory\n");

	err = dmamem_heap_alloc_aligned(&rt.heap, NBYTES, 4096, &payload_off);
	if (err) {
		printf("# FAILED: dmamem_heap_alloc_aligned(payload); err(%d)\n", err);
		goto exit;
	}
	payload_alive = 1;
	payload = (char *)view.heap_base + payload_off;
	memset(payload, 0, NBYTES);

	cmd.opc = 0x2;
	cmd.nsid = 1;
	cmd.prp1 = dmamem_va_to_iova(&view.dmem, payload);
	cmd.cdw10 = 0;
	cmd.cdw12 = 0;

	err = nvme_qpair_submit_sync(&rebuilt, &cmd, view.ctrlr.base.timeout_ms, &cpl);
	if (err) {
		printf("# FAILED: nvme_qpair_submit_sync(); err(%d)\n", err);
		goto exit;
	}
	{
		uint8_t sc = (cpl.status & 0x1FE) >> 1;
		uint8_t sct = (cpl.status & 0xE00) >> 9;

		if (sc || sct) {
			printf("# FAILED: read; sct(0x%x) sc(0x%x)\n", sct, sc);
			err = -EIO;
			goto exit;
		}
	}

	printf("# LGTM: read LBA 0 through the rebuilt queue\n");

exit:
	if (rebuilt.rpool) {
		view_qpair_release(&rebuilt);
	}
	view_term(&view);
	if (payload_alive) {
		dmamem_heap_free(&rt.heap, payload_off);
	}
	if (qpair_alive) {
		nvme_controller_delete_io_qpair_dmamem(&rt.ctrlr, &qpair, &rt.heap, sq_off, cq_off,
						       prp_off);
	}
	runtime_term(&rt);

	return err ? 1 : 0;
}
