// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * Reference NVMe driver on uPCIe
 * ==============================
 *
 * One driver, three ways to reach the controller. The controller is opened through whichever
 * kernel driver it is bound to, uio_pci_generic or vfio-pci, and under vfio-pci through iommufd
 * where /dev/iommu is usable and through a type1 container otherwise (UPCIE_VFIO_MODE=type1 or
 * iommufd forces the choice). What differs between the three is how BAR0 is reached and how the
 * memory the controller DMAs against is made addressable: physical addresses under
 * uio_pci_generic, a VFIO_IOMMU_MAP_DMA under type1, an IOAS under iommufd. Each of them is
 * wrapped as a dmamem, and from the dmamem_heap onwards the driver is the same code: the same
 * admin queue, the same I/O queue pair, the same PRP construction.
 *
 * Usage:
 *   upcie_nvme_driver <PCI-BDF>
 */

#include <linux/limits.h>

#define _UPCIE_WITH_NVME
#include <upcie/upcie.h>

#define HEAP_NBYTES (16ULL * 1024 * 1024)
#define HUGEPGSZ (2ULL * 1024 * 1024)

enum nvme_backend {
	NVME_BACKEND_UIO = 0,      ///< uio_pci_generic, physical addresses
	NVME_BACKEND_VFIO_TYPE1,   ///< vfio-pci, type1 container
	NVME_BACKEND_VFIO_IOMMUFD, ///< vfio-pci, iommufd
};

static int
iommufd_available(void)
{
#ifdef IOMMU_IOAS_MAP_FILE
	return access("/dev/iommu", R_OK | W_OK) == 0;
#else
	return 0;
#endif
}

/*
 * Pick the vfio-pci backend from UPCIE_VFIO_MODE (auto|type1|iommufd); auto uses iommufd when
 * /dev/iommu is usable, otherwise a type1 container.
 */
static enum nvme_backend
resolve_vfio_backend(void)
{
	const char *mode = getenv("UPCIE_VFIO_MODE");

	if (mode && !strcmp(mode, "type1")) {
		return NVME_BACKEND_VFIO_TYPE1;
	}
	if (mode && !strcmp(mode, "iommufd")) {
		return NVME_BACKEND_VFIO_IOMMUFD;
	}
	if (mode && *mode && strcmp(mode, "auto")) {
		printf("WARN: UPCIE_VFIO_MODE='%s' unknown; using auto\n", mode);
	}
	return iommufd_available() ? NVME_BACKEND_VFIO_IOMMUFD : NVME_BACKEND_VFIO_TYPE1;
}

/**
 * Everything the driver holds, whichever backend opened the controller
 *
 * The attachment differs per backend and lives in the union; the dmamem, the heap on it and the
 * controller are the same three things in every case.
 */
struct nvme {
	enum nvme_backend backend;

	struct hostmem_config config; ///< uio and type1: the hugepage the heap sits on
	struct hostmem_hugepage hp;
	struct iommufd iommufd; ///< iommufd: the IOAS the heap is mapped into

	struct dmamem dmem;
	struct dmamem_heap heap;
	struct nvme_controller ctrlr;

	union {
		struct nvme_dmamem_uio_ctx uio;
		struct {
			struct vfio_container container;
			struct vfio_group group;
			struct nvme_dmamem_type1_ctx ctx;
		} type1;
		struct nvme_dmamem_vfio_ctx iommufd;
	} attach;

	struct nvme_qpair ioq;
	size_t ioq_sq, ioq_cq, ioq_prp;

	int hp_alive, iommufd_alive, ioas_alive, container_alive, group_alive;
	int dmem_alive, heap_alive, ctrlr_alive, ioq_alive;
};

static int
device_get_driver_name(const char *bdf, char *driver_name, size_t driver_name_len)
{
	char path[PATH_MAX] = {0};
	char link[PATH_MAX] = {0};
	ssize_t nbytes;
	char *base;

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/driver", bdf);

	nbytes = readlink(path, link, sizeof(link) - 1);
	if (nbytes < 0) {
		return -errno;
	}

	base = strrchr(link, '/');
	if (!base || !base[1]) {
		return -EINVAL;
	}

	snprintf(driver_name, driver_name_len, "%s", base + 1);

	return 0;
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

static void
nvme_term(struct nvme *nvme)
{
	if (nvme->ioq_alive) {
		nvme_controller_delete_io_qpair_dmamem(&nvme->ctrlr, &nvme->ioq, &nvme->heap,
						       nvme->ioq_sq, nvme->ioq_cq, nvme->ioq_prp);
		nvme->ioq_alive = 0;
	}

	if (nvme->ctrlr_alive) {
		switch (nvme->backend) {
		case NVME_BACKEND_UIO:
			nvme_controller_close_dmamem_uio(&nvme->ctrlr, &nvme->attach.uio,
							 &nvme->heap);
			break;
		case NVME_BACKEND_VFIO_TYPE1:
			nvme_controller_close_dmamem_type1(&nvme->ctrlr, &nvme->attach.type1.ctx,
							   &nvme->heap);
			break;
		case NVME_BACKEND_VFIO_IOMMUFD:
			nvme_controller_close_dmamem_vfio(&nvme->ctrlr, &nvme->attach.iommufd,
							  &nvme->heap);
			break;
		}
		nvme->ctrlr_alive = 0;
	}

	if (nvme->heap_alive) {
		dmamem_heap_term(&nvme->heap);
		nvme->heap_alive = 0;
	}
	if (nvme->dmem_alive) {
		dmamem_destroy(&nvme->dmem);
		nvme->dmem_alive = 0;
	}
	if (nvme->group_alive) {
		vfio_group_close(&nvme->attach.type1.group);
		nvme->group_alive = 0;
	}
	if (nvme->container_alive) {
		vfio_container_close(&nvme->attach.type1.container);
		nvme->container_alive = 0;
	}
	if (nvme->hp_alive) {
		hostmem_hugepage_free(&nvme->hp);
		nvme->hp_alive = 0;
	}
	if (nvme->ioas_alive) {
		iommufd_destroy(&nvme->iommufd, nvme->iommufd.ioas_id);
		nvme->ioas_alive = 0;
	}
	if (nvme->iommufd_alive) {
		iommufd_close(&nvme->iommufd);
		nvme->iommufd_alive = 0;
	}
}

/**
 * A hugepage for the heap to sit on; uio and type1 both DMA against it, by different addresses
 */
static int
hugepage_init(struct nvme *nvme)
{
	int err;

	err = hostmem_config_init(&nvme->config);
	if (err) {
		printf("FAILED: hostmem_config_init(); err(%d)\n", err);
		return err;
	}

	err = hostmem_hugepage_alloc(HEAP_NBYTES, &nvme->hp, &nvme->config);
	if (err) {
		printf("FAILED: hostmem_hugepage_alloc(); err(%d); are hugepages reserved?\n",
		       err);
		return err;
	}
	nvme->hp_alive = 1;

	return 0;
}

/**
 * uio_pci_generic: the controller DMAs against physical addresses, read from pagemap
 */
static int
nvme_open_uio(struct nvme *nvme, const char *bdf)
{
	int err;

	err = hugepage_init(nvme);
	if (err) {
		return err;
	}

	err = dmamem_from_hostmem_registry(&nvme->dmem, &nvme->hp, 0);
	if (err) {
		printf("FAILED: dmamem_from_hostmem_registry(); err(%d); missing CAP_SYS_ADMIN?\n",
		       err);
		return err;
	}
	nvme->dmem_alive = 1;

	err = dmamem_heap_init(&nvme->heap, &nvme->dmem, 4096);
	if (err) {
		printf("FAILED: dmamem_heap_init(); err(%d)\n", err);
		return err;
	}
	nvme->heap_alive = 1;

	nvme_dmamem_uio_ctx_init(&nvme->attach.uio);
	err = nvme_controller_open_dmamem_uio(&nvme->ctrlr, &nvme->attach.uio, &nvme->heap, bdf);
	if (err) {
		printf("FAILED: nvme_controller_open_dmamem_uio(%s); err(%d)\n", bdf, err);
		return err;
	}
	nvme->ctrlr_alive = 1;

	return 0;
}

/**
 * vfio-pci with a type1 container: one VFIO_IOMMU_MAP_DMA over the hugepage, at IOVA 0
 */
static int
nvme_open_type1(struct nvme *nvme, const char *bdf)
{
	struct vfio_container *container = &nvme->attach.type1.container;
	struct vfio_group *group = &nvme->attach.type1.group;
	int api_version = 0;
	int group_id = -1;
	int err;

	err = hugepage_init(nvme);
	if (err) {
		return err;
	}

	err = vfio_container_open(container);
	if (err) {
		printf("FAILED: vfio_container_open(); err(%d)\n", err);
		return err;
	}
	nvme->container_alive = 1;

	err = vfio_device_get_iommu_group_id(bdf, &group_id);
	if (err) {
		printf("FAILED: vfio_device_get_iommu_group_id(%s); err(%d)\n", bdf, err);
		return err;
	}

	err = vfio_group_open(group_id, group);
	if (err) {
		printf("FAILED: vfio_group_open(%d); err(%d)\n", group_id, err);
		return err;
	}
	nvme->group_alive = 1;

	err = vfio_group_get_status(group);
	if (err < 0) {
		printf("FAILED: vfio_group_get_status(); errno(%d)\n", errno);
		return -errno;
	}
	if (!(group->status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
		printf("FAILED: iommu group %d is not viable; is every device in it bound to "
		       "vfio-pci?\n",
		       group_id);
		return -EBUSY;
	}

	err = vfio_group_set_container(group, container);
	if (err < 0) {
		printf("FAILED: vfio_group_set_container(); errno(%d)\n", errno);
		return -errno;
	}

	err = vfio_get_api_version(container, &api_version);
	if (err) {
		printf("FAILED: vfio_get_api_version(); err(%d)\n", err);
		return err;
	}
	if (api_version != VFIO_API_VERSION) {
		printf("FAILED: VFIO_API_VERSION(%d) != %d\n", api_version, VFIO_API_VERSION);
		return -EINVAL;
	}
	if (!vfio_check_extension(container, VFIO_TYPE1_IOMMU)) {
		printf("FAILED: VFIO_TYPE1_IOMMU is not supported\n");
		return -ENOTSUP;
	}

	err = vfio_set_iommu(container, VFIO_TYPE1_IOMMU);
	if (err < 0) {
		printf("FAILED: vfio_set_iommu(TYPE1); errno(%d)\n", errno);
		return -errno;
	}

	err = dmamem_from_hostmem_type1(&nvme->dmem, container, 0, &nvme->hp);
	if (err) {
		printf("FAILED: dmamem_from_hostmem_type1(); err(%d)\n", err);
		return err;
	}
	nvme->dmem_alive = 1;

	err = dmamem_heap_init(&nvme->heap, &nvme->dmem, 4096);
	if (err) {
		printf("FAILED: dmamem_heap_init(); err(%d)\n", err);
		return err;
	}
	nvme->heap_alive = 1;

	nvme_dmamem_type1_ctx_init(&nvme->attach.type1.ctx);
	err = nvme_controller_open_dmamem_type1(&nvme->ctrlr, &nvme->attach.type1.ctx, container,
						group, &nvme->heap, bdf);
	if (err) {
		printf("FAILED: nvme_controller_open_dmamem_type1(%s); err(%d)\n", bdf, err);
		return err;
	}
	nvme->ctrlr_alive = 1;

	return 0;
}

/**
 * vfio-pci with iommufd: a memfd of hugepages mapped into an IOAS the controller is attached to
 */
static int
nvme_open_iommufd(struct nvme *nvme, const char *bdf)
{
	char cdev_path[PATH_MAX] = {0};
	int err;

	err = resolve_vfio_cdev(bdf, cdev_path, sizeof(cdev_path));
	if (err) {
		printf("FAILED: resolve_vfio_cdev(%s); err(%d)\n", bdf, err);
		return err;
	}

	err = iommufd_open(&nvme->iommufd);
	if (err) {
		printf("FAILED: iommufd_open(); err(%d)\n", err);
		return err;
	}
	nvme->iommufd_alive = 1;

	err = iommufd_ioas_alloc(&nvme->iommufd);
	if (err) {
		printf("FAILED: iommufd_ioas_alloc(); err(%d)\n", err);
		return err;
	}
	nvme->ioas_alive = 1;

	err = dmamem_from_memfd(&nvme->dmem, &nvme->iommufd, HEAP_NBYTES, HUGEPGSZ);
	if (err) {
		printf("FAILED: dmamem_from_memfd(); err(%d); are hugepages reserved?\n", err);
		return err;
	}
	nvme->dmem_alive = 1;

	err = dmamem_heap_init(&nvme->heap, &nvme->dmem, 4096);
	if (err) {
		printf("FAILED: dmamem_heap_init(); err(%d)\n", err);
		return err;
	}
	nvme->heap_alive = 1;

	nvme_dmamem_vfio_ctx_init(&nvme->attach.iommufd);
	err = nvme_controller_open_dmamem_vfio(&nvme->ctrlr, &nvme->attach.iommufd, &nvme->iommufd,
					       &nvme->heap, cdev_path);
	if (err) {
		printf("FAILED: nvme_controller_open_dmamem_vfio(%s); err(%d)\n", cdev_path, err);
		return err;
	}
	nvme->ctrlr_alive = 1;

	return 0;
}

/**
 * From here on the backend does not matter: a buffer from the heap, a command on the admin queue
 */
static int
nvme_identify(struct nvme *nvme)
{
	struct nvme_completion cpl = {0};
	struct nvme_command cmd = {0};
	size_t off = 0;
	uint8_t *buf;
	int err;

	err = dmamem_heap_alloc_aligned(&nvme->heap, 4096, 4096, &off);
	if (err) {
		printf("FAILED: dmamem_heap_alloc_aligned(identify); err(%d)\n", err);
		return err;
	}
	buf = dmamem_heap_at_va(&nvme->heap, off);
	memset(buf, 0, 4096);

	cmd.opc = 0x6; ///< IDENTIFY
	cmd.cid = 1;
	cmd.prp1 = dmamem_heap_at_iova(&nvme->heap, off);
	cmd.cdw10 = 1; ///< CNS=1: Identify Controller

	err = nvme_qpair_submit_sync(&nvme->ctrlr.aq, &cmd, nvme->ctrlr.timeout_ms, &cpl);
	if (err) {
		/* Unreaped, so the controller may still write here; leave it allocated. */
		printf("FAILED: nvme_qpair_submit_sync(identify); err(%d)\n", err);
		return err;
	}
	if ((cpl.status >> 1) & 0x7FF) {
		printf("FAILED: IDENTIFY status(0x%x)\n", cpl.status);
		err = -EIO;
		goto exit;
	}

	printf("SN('%.*s')\n", 20, buf + 4);
	printf("MN('%.*s')\n", 40, buf + 24);

exit:
	dmamem_heap_free(&nvme->heap, off);

	return err;
}

static int
nvme_init(struct nvme *nvme, const char *bdf)
{
	char driver_name[NAME_MAX + 1] = {0};
	int err;

	err = device_get_driver_name(bdf, driver_name, sizeof(driver_name));
	if (err) {
		printf("FAILED: device_get_driver_name(%s); err(%d)\n", bdf, err);
		return err;
	}

	if (!strcmp(driver_name, "uio_pci_generic")) {
		nvme->backend = NVME_BACKEND_UIO;
		err = nvme_open_uio(nvme, bdf);
	} else if (!strcmp(driver_name, "vfio-pci")) {
		nvme->backend = resolve_vfio_backend();
		err = (nvme->backend == NVME_BACKEND_VFIO_IOMMUFD) ? nvme_open_iommufd(nvme, bdf)
								   : nvme_open_type1(nvme, bdf);
	} else {
		printf("FAILED: unsupported driver '%s'\n", driver_name);
		return -ENOTSUP;
	}
	if (err) {
		return err;
	}

	err = nvme_identify(nvme);
	if (err) {
		return err;
	}

	err = nvme_controller_create_io_qpair_dmamem(&nvme->ctrlr, &nvme->ioq, 32, &nvme->heap,
						     &nvme->ioq_sq, &nvme->ioq_cq, &nvme->ioq_prp);
	if (err) {
		printf("FAILED: nvme_controller_create_io_qpair_dmamem(); err(%d)\n", err);
		return err;
	}
	nvme->ioq_alive = 1;

	return 0;
}

int
main(int argc, char **argv)
{
	struct nvme nvme = {0};
	int err;

	if (argc != 2) {
		printf("Usage: %s <PCI-BDF>\n", argv[0]);
		return 1;
	}

	err = nvme_init(&nvme, argv[1]);
	if (err) {
		printf("FAILED: nvme_init(); err(%d)\n", err);
	}

	nvme_term(&nvme);

	return err ? 1 : 0;
}
