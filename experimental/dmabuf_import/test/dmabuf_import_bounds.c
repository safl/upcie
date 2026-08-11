// SPDX-License-Identifier: GPL-2.0
/*
 * dmabuf_import_bounds - regression test for the DMABUF_IMPORT_GET_MAP bound
 * =========================================================================
 *
 * DMABUF_IMPORT_GET_MAP must write at most get_map.count entries into the
 * userspace dma_arr. The original loop guarded with `i > count`, which still
 * let index `count` be written (one past the end). This test detects that.
 *
 * It imports a multi-segment dma-buf, then allocates room for `count` entries
 * but tells the ioctl there are only `count - 1`, with a canary planted in the
 * extra slot. A correct kernel never touches that slot; the buggy one-past
 * write overwrites the canary.
 *
 * Target-side (needs the stock /dev/udmabuf for UDMABUF_CREATE and the module
 * at /dev/dmabuf_import). Exit 0 on OK, 1 on the out-of-bounds write.
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
#include <unistd.h>

/* <linux/udmabuf.h> above provides UDMABUF_CREATE; the import ioctls and the
 * device-path macro come from the module header. */
#include "../module/dmabuf_import.h"

#define PAGES 8
#define BUF_SIZE (PAGES * 4096)
#define CANARY 0xDEADBEEFCAFEBABEULL

static int create_udmabuf(int udmabuf_fd, size_t size) {
  struct udmabuf_create create;
  int memfd, dmabuf_fd;

  memfd = memfd_create("udmabuf-bounds", MFD_ALLOW_SEALING);
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
  if (dmabuf_fd < 0) {
    perror("UDMABUF_CREATE");
    return -1;
  }
  return dmabuf_fd;
}

int main(void) {
  struct dmabuf_import_attach attach;
  struct dmabuf_import_get_map *gm;
  int udmabuf_fd, import_fd, dmabuf_fd, count, overwritten;

  udmabuf_fd = open("/dev/udmabuf", O_RDWR);
  if (udmabuf_fd < 0) {
    perror("open /dev/udmabuf");
    return 2;
  }
  import_fd = open(DMABUF_IMPORT_DEVPATH, O_RDWR);
  if (import_fd < 0) {
    perror("open /dev/dmabuf_import");
    return 2;
  }

  dmabuf_fd = create_udmabuf(udmabuf_fd, BUF_SIZE);
  if (dmabuf_fd < 0)
    return 2;

  memset(&attach, 0, sizeof(attach));
  attach.fd = dmabuf_fd;
  if (ioctl(import_fd, DMABUF_IMPORT_ATTACH, &attach)) {
    perror("DMABUF_IMPORT_ATTACH");
    return 2;
  }
  count = attach.count;
  printf("attached: %d mapping(s)\n", count);
  if (count < 2) {
    printf("need >= 2 mappings for a bounds test, got %d; skipping\n", count);
    return 2;
  }

  /* Room for `count` entries; canary the last slot; but ask for count - 1. */
  gm = calloc(1, sizeof(*gm) + (size_t)count * sizeof(struct dmabuf_import_dma_map));
  if (!gm) {
    perror("calloc");
    return 2;
  }
  gm->fd = dmabuf_fd;
  gm->count = count - 1;
  gm->dma_arr[count - 1].dma_addr = CANARY;
  gm->dma_arr[count - 1].dma_len = CANARY;

  if (ioctl(import_fd, DMABUF_IMPORT_GET_MAP, gm)) {
    perror("DMABUF_IMPORT_GET_MAP");
    free(gm);
    return 2;
  }

  overwritten = gm->dma_arr[count - 1].dma_addr != CANARY ||
                gm->dma_arr[count - 1].dma_len != CANARY;
  printf("requested %d, canary slot [%d]: addr=0x%llx len=0x%llx\n", count - 1,
         count - 1, (unsigned long long)gm->dma_arr[count - 1].dma_addr,
         (unsigned long long)gm->dma_arr[count - 1].dma_len);

  if (ioctl(import_fd, DMABUF_IMPORT_DETACH, &dmabuf_fd))
    perror("DMABUF_IMPORT_DETACH (non-fatal)");
  free(gm);

  printf("%s\n", overwritten ? "BOUNDS: FAIL (one-past-the-end write)"
                             : "BOUNDS: OK");
  return overwritten ? 1 : 0;
}
