// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

#define _UPCIE_WITH_NVME
#include <upcie/upcie.h>

enum nvme_backend {
	NVME_BACKEND_SYSFS = 0,
	NVME_BACKEND_VFIO,
};

struct rte {
	struct hostmem_config config;
	struct hostmem_heap heap;
};

struct nvme {
	struct nvme_controller ctrlr;
	struct nvme_qpair ioq;
	struct vfio_ctx vfio;
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

static void
nvme_cleanup(struct nvme *nvme)
{
	if (nvme->ioq.rpool) {
		nvme_qpair_term(&nvme->ioq);
		memset(&nvme->ioq, 0, sizeof(nvme->ioq));
	}

	if (nvme->backend == NVME_BACKEND_VFIO) {
		nvme_controller_close_vfio(&nvme->ctrlr, &nvme->vfio);
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

	if (!strcmp(driver_name, "vfio-pci")) {
		nvme->backend = NVME_BACKEND_VFIO;
		err = nvme_controller_open_vfio(&nvme->ctrlr, &nvme->vfio, bdf, &rte->heap);
	} else if (!strcmp(driver_name, "uio_pci_generic")) {
		nvme->backend = NVME_BACKEND_SYSFS;
		err = nvme_controller_open(&nvme->ctrlr, bdf, &rte->heap);
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

		if (nvme->backend == NVME_BACKEND_VFIO) {
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
