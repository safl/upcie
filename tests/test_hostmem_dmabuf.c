#include <upcie/upcie.h>

int get_dmabuf_fd(int *dmabuf_fd, int memfd, size_t size)
{
	struct udmabuf_create create;
	int udmabuf_fd, err;

	udmabuf_fd = open("/dev/udmabuf", O_RDWR);

	memset(&create, 0, sizeof(create));
	create.memfd = memfd;
	create.offset = 0;
	create.size = size;
	*dmabuf_fd = ioctl(udmabuf_fd, UDMABUF_CREATE, &create);
	if (*dmabuf_fd < 0) {
		err = -errno;
		printf("# FAILED: ioctl(UDMABUF_CREATE); errno(%d)\n", err);
		return err;
	}

	close(udmabuf_fd);
	return 0;
}

int main(void)
{
	struct hostmem_config config = {0};
	struct dmabuf dmabuf = {0};
	const size_t npages = 8;
	size_t size;
	uint64_t *phys_lut;
	size_t nphys;
	int memfd, dmabuf_fd, err;

	err = hostmem_config_init(&config);
	if (err) {
		printf("# FAILED: hostmem_config_init(); err(%d)\n", err);
		return err;
	}

	size = npages * config.pagesize;

	memfd = memfd_create("upcie-dmabuf-test", MFD_ALLOW_SEALING);
	if (memfd < 0) {
		err = -errno;
		printf("# FAILED: memfd_create(); err(%d)\n", err);
		return err;
	}

	err = fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK);
	if (err) {
		err = -errno;
		printf("# FAILED: fcntl(F_ADD_SEALS); err(%d)\n", err);
		return err;
	}

	err = ftruncate(memfd, size);
	if (err) {
		err = -errno;
		printf("# FAILED: ftruncate(); err(%d)\n", err);
		return err;
	}

	err = get_dmabuf_fd(&dmabuf_fd, memfd, size);
	if (err) {
		printf("# FAILED: get_dmabuf_fd(); err(%d)\n", err);
		return err;
	}

	nphys = size / config.pagesize;

	phys_lut = calloc(nphys, sizeof(uint64_t));
	if (!phys_lut) {
		err = -errno;
		printf("# FAILED: calloc(phys_lut); errno(%d)\n", err);
		return err;
	}

	err = dmabuf_attach(dmabuf_fd, &dmabuf);
	if (err) {
		printf("# FAILED: dmabuf_attach(); err(%d)\n", err);
		free(phys_lut);
		return err;
	}

	dmabuf_pp(&dmabuf);

	err = dmabuf_get_lut(&dmabuf, nphys, phys_lut, config.pagesize);
	if (err) {
		printf("# FAILED: dmabuf_get_lut(); err(%d)\n", err);
		free(phys_lut);
		return err;
	}

	printf("LUT:\n");
	printf("  nphys: %zu\n", nphys);
	printf("  phys_lut:\n");
	for (size_t i = 0; i < nphys; i++) {
		printf("  - 0x%" PRIx64 "\n", phys_lut[i]);
	}

	dmabuf_detach(&dmabuf);
	free(phys_lut);
	return 0;
}
