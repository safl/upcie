// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

#include <linux/limits.h>

#define _UPCIE_WITH_NVME
#include <upcie/upcie.h>

enum nvme_backend {
	NVME_BACKEND_SYSFS = 0,    ///< uio_pci_generic + hostmem
	NVME_BACKEND_VFIO_TYPE1,   ///< vfio-pci + vfio type1 container + hostmem
	NVME_BACKEND_VFIO_IOMMUFD, ///< vfio-pci + iommufd + dmamem
};

enum vfio_mode {
	VFIO_MODE_AUTO = 0, ///< iommufd if /dev/iommu is usable, otherwise type1
	VFIO_MODE_TYPE1,    ///< force legacy vfio type1 container
	VFIO_MODE_IOMMUFD,  ///< force iommufd + dmamem
};

static enum vfio_mode
vfio_mode_from_env(void)
{
	const char *v = getenv("UPCIE_VFIO_MODE");

	if (!v || !*v || !strcmp(v, "auto")) {
		return VFIO_MODE_AUTO;
	}
	if (!strcmp(v, "type1")) {
		return VFIO_MODE_TYPE1;
	}
	if (!strcmp(v, "iommufd")) {
		return VFIO_MODE_IOMMUFD;
	}
	printf("WARN: UPCIE_VFIO_MODE='%s' unknown; using auto\n", v);
	return VFIO_MODE_AUTO;
}

static int
iommufd_available(void)
{
#ifdef IOMMU_IOAS_MAP_FILE
	return access("/dev/iommu", R_OK | W_OK) == 0;
#else
	return 0;
#endif
}

static enum nvme_backend
resolve_vfio_backend(void)
{
	switch (vfio_mode_from_env()) {
	case VFIO_MODE_TYPE1:
		return NVME_BACKEND_VFIO_TYPE1;
	case VFIO_MODE_IOMMUFD:
		return NVME_BACKEND_VFIO_IOMMUFD;
	case VFIO_MODE_AUTO:
	default:
		return iommufd_available() ? NVME_BACKEND_VFIO_IOMMUFD : NVME_BACKEND_VFIO_TYPE1;
	}
}

struct rte {
	struct hostmem_config config;
	struct hostmem_heap heap;
};

struct nvme_dmamem_state {
	struct iommufd iommufd;
	uint32_t ioas_id;
	struct dmamem dmem;
	struct dmamem_heap heap;
	struct nvme_dmamem_ctx ctx;
	size_t buf_off;
	void *buf;
	int iommufd_alive;
	int ioas_alive;
	int dmem_alive;
	int heap_alive;
	int ctrlr_alive;
};

struct nvme {
	struct nvme_controller ctrlr;
	struct nvme_qpair ioq;
	struct vfio_ctx vfio;
	struct nvme_dmamem_state dm;
	enum nvme_backend backend;
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
nvme_cleanup(struct nvme *nvme)
{
	if (nvme->ioq.rpool) {
		nvme_controller_delete_io_qpair(&nvme->ctrlr, &nvme->ioq);
		memset(&nvme->ioq, 0, sizeof(nvme->ioq));
	}

	if (nvme->backend == NVME_BACKEND_VFIO_TYPE1) {
		nvme_controller_close_vfio(&nvme->ctrlr, &nvme->vfio);
		return;
	}

	if (nvme->backend == NVME_BACKEND_VFIO_IOMMUFD) {
		if (nvme->dm.buf) {
			dmamem_heap_free(&nvme->dm.heap, nvme->dm.buf_off);
			nvme->dm.buf = NULL;
		}
		if (nvme->dm.ctrlr_alive) {
			nvme_controller_close_dmamem(&nvme->ctrlr, &nvme->dm.ctx, &nvme->dm.heap);
			nvme->dm.ctrlr_alive = 0;
		}
		if (nvme->dm.heap_alive) {
			dmamem_heap_term(&nvme->dm.heap);
			nvme->dm.heap_alive = 0;
		}
		if (nvme->dm.dmem_alive) {
			dmamem_destroy(&nvme->dm.dmem);
			nvme->dm.dmem_alive = 0;
		}
		if (nvme->dm.ioas_alive) {
			iommufd_destroy(&nvme->dm.iommufd, nvme->dm.ioas_id);
			nvme->dm.ioas_alive = 0;
		}
		if (nvme->dm.iommufd_alive) {
			iommufd_close(&nvme->dm.iommufd);
			nvme->dm.iommufd_alive = 0;
		}
		return;
	}

	nvme_controller_close(&nvme->ctrlr);
}

int
rte_init(struct rte *rte)
{
	int err;

	err = hostmem_config_init(&rte->config);
	if (err) {
		printf("FAILED: hostmem_config_init(); err(%d)\n", err);
		return err;
	}

	err = hostmem_heap_init(&rte->heap, 1024 * 1024 * 128ULL, &rte->config);
	if (err) {
		printf("FAILED: hostmem_heap_init(); err(%d)\n", err);
		return -err;
	}

	return 0;
}

static int
nvme_open_dmamem(struct nvme *nvme, const char *bdf)
{
	char cdev_path[PATH_MAX] = {0};
	size_t hugepgsz = 2ULL * 1024 * 1024;
	size_t heap_size = hugepgsz * 4;
	int err;

	err = resolve_vfio_cdev(bdf, cdev_path, sizeof(cdev_path));
	if (err) {
		printf("FAILED: resolve_vfio_cdev(%s); err(%d)\n", bdf, err);
		return err;
	}

	err = iommufd_open(&nvme->dm.iommufd);
	if (err) {
		printf("FAILED: iommufd_open(); err(%d)\n", err);
		return err;
	}
	nvme->dm.iommufd_alive = 1;

	err = iommufd_ioas_alloc(&nvme->dm.iommufd, &nvme->dm.ioas_id);
	if (err) {
		printf("FAILED: iommufd_ioas_alloc(); err(%d)\n", err);
		goto fail;
	}
	nvme->dm.ioas_alive = 1;

	err = dmamem_from_memfd(&nvme->dm.dmem, &nvme->dm.iommufd, nvme->dm.ioas_id, heap_size,
				hugepgsz);
	if (err) {
		printf("FAILED: dmamem_from_memfd(); err(%d)\n", err);
		goto fail;
	}
	nvme->dm.dmem_alive = 1;

	err = dmamem_heap_init(&nvme->dm.heap, &nvme->dm.dmem, 4096);
	if (err) {
		printf("FAILED: dmamem_heap_init(); err(%d)\n", err);
		goto fail;
	}
	nvme->dm.heap_alive = 1;

	err = nvme_controller_open_dmamem(&nvme->ctrlr, &nvme->dm.ctx, &nvme->dm.iommufd,
					  nvme->dm.ioas_id, &nvme->dm.heap, cdev_path);
	if (err) {
		printf("FAILED: nvme_controller_open_dmamem(); err(%d)\n", err);
		goto fail;
	}
	nvme->dm.ctrlr_alive = 1;

	err = dmamem_heap_alloc_aligned(&nvme->dm.heap, 4096, 4096, &nvme->dm.buf_off);
	if (err) {
		printf("FAILED: dmamem_heap_alloc_aligned(identify buf); err(%d)\n", err);
		goto fail;
	}
	nvme->dm.buf = dmamem_heap_at_va(&nvme->dm.heap, nvme->dm.buf_off);
	memset(nvme->dm.buf, 0, 4096);

	return 0;

fail:
	nvme_cleanup(nvme);
	return err;
}

static int
nvme_identify_dmamem(struct nvme *nvme)
{
	struct nvme_completion cpl = {0};
	struct nvme_command cmd = {0};
	int err;

	cmd.opc = 0x6; // IDENTIFY
	cmd.cid = 1;
	cmd.prp1 = dmamem_heap_at_iova(&nvme->dm.heap, nvme->dm.buf_off);
	cmd.cdw10 = 1; // CNS=1: Identify Controller

	err = nvme_qpair_enqueue(&nvme->ctrlr.aq, &cmd);
	if (err) {
		return err;
	}
	nvme_qpair_sqdb_update(&nvme->ctrlr.aq);

	err = nvme_qpair_reap_cpl(&nvme->ctrlr.aq, nvme->ctrlr.timeout_ms, &cpl);
	if (err) {
		return err;
	}
	if ((cpl.status >> 1) & 0x7FF) {
		printf("FAILED: IDENTIFY CQE status(0x%x)\n", cpl.status);
		return -EIO;
	}

	printf("SN('%.*s')\n", 20, ((uint8_t *)nvme->dm.buf) + 4);
	printf("MN('%.*s')\n", 40, ((uint8_t *)nvme->dm.buf) + 24);

	return 0;
}

int
nvme_init(struct nvme *nvme, const char *bdf, struct rte *rte)
{
	char driver_name[NAME_MAX + 1] = {0};
	struct nvme_completion cpl = {0};
	struct nvme_command cmd = {0};
	int err;

	err = device_get_driver_name(bdf, driver_name, sizeof(driver_name));
	if (err) {
		printf("FAILED: device_get_driver_name(); err(%d)\n", err);
		return -err;
	}

	if (!strcmp(driver_name, "uio_pci_generic")) {
		nvme->backend = NVME_BACKEND_SYSFS;
		err = nvme_controller_open(&nvme->ctrlr, bdf, &rte->heap);
	} else if (!strcmp(driver_name, "vfio-pci")) {
		nvme->backend = resolve_vfio_backend();
		if (nvme->backend == NVME_BACKEND_VFIO_IOMMUFD) {
			err = nvme_open_dmamem(nvme, bdf);
			if (err) {
				return -err;
			}
			err = nvme_identify_dmamem(nvme);
			if (err) {
				printf("FAILED: nvme_identify_dmamem(); err(%d)\n", err);
				nvme_cleanup(nvme);
				return -err;
			}
			return 0;
		}
		err = nvme_controller_open_vfio(&nvme->ctrlr, &nvme->vfio, bdf, &rte->heap);
	} else {
		printf("FAILED: unsupported driver '%s'\n", driver_name);
		return -ENOTSUP;
	}
	if (err) {
		printf("FAILED: nvme_device_open(); err(%d)\n", err);
		return -err;
	}

	cmd.opc = 0x6; ///< IDENTIFY
	cmd.cdw10 = 1; // CNS=1: Identify Controller

	err = nvme_qpair_submit_sync_contig_prps(&nvme->ctrlr.aq, nvme->ctrlr.heap,
						 nvme->ctrlr.buf, 4096, &cmd,
						 nvme->ctrlr.timeout_ms, &cpl);
	if (err) {
		printf("FAILED: nvme_qpair_submit_sync(); err(%d)\n", err);
		nvme_cleanup(nvme);
		return err;
	}

	printf("SN('%.*s')\n", 20, ((uint8_t *)nvme->ctrlr.buf) + 4);
	printf("MN('%.*s')\n", 40, ((uint8_t *)nvme->ctrlr.buf) + 24);

	err = nvme_controller_create_io_qpair(&nvme->ctrlr, &nvme->ioq, 32);
	if (err) {
		printf("FAILED: nvme_device_create_io_qpair(); err(%d)\n", err);

		if (nvme->backend == NVME_BACKEND_VFIO_TYPE1) {
			nvme_controller_close_vfio(&nvme->ctrlr, &nvme->vfio);
		} else {
			nvme_controller_close(&nvme->ctrlr);
		}

		return err;
	}

	return 0;
}

int
main(int argc, char **argv)
{
	struct nvme nvme = {0};
	struct rte rte = {0};
	int err;

	if (argc != 2) {
		printf("Usage: %s <PCI-BDF>\n", argv[0]);
		return 1;
	}

	err = rte_init(&rte);
	if (err) {
		printf("FAILED: rte_init();");
		return -err;
	}

	err = nvme_init(&nvme, argv[1], &rte);
	if (err) {
		printf("FAILED: nvme_init();");
		hostmem_heap_term(&rte.heap);
		return -err;
	}

	nvme_cleanup(&nvme);
	hostmem_heap_term(&rte.heap);

	return err;
}
