// SPDX-License-Identifier: GPL-2.0
/*
 * udmabuf_import_parity - functional parity check for the out-of-tree module
 * =========================================================================
 *
 * Creates a memfd-backed dma-buf via the stock /dev/udmabuf (UDMABUF_CREATE),
 * then imports the SAME dma-buf twice and compares the returned DMA maps:
 *
 *   1. via /dev/udmabuf         - the in-tree (patched) importer ioctls
 *   2. via /dev/udmabuf_import  - the out-of-tree module under test
 *
 * If both paths return identical (dma_addr, dma_len) arrays, the module is a
 * behavioural drop-in for the in-tree patch. Exit 0 on match, 1 on mismatch.
 *
 * This is a target-side validation for a box that runs the *patched* kernel
 * (so /dev/udmabuf carries the import ioctls and <linux/udmabuf.h> defines the
 * import structs). It intentionally uses only the system header to avoid a
 * struct redefinition against the module's own udmabuf_import.h. The shipped
 * examples, by contrast, target stock boxes and include udmabuf_import.h.
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

#define BUF_SIZE (8 * 4096)

static int create_udmabuf(int udmabuf_fd, size_t size) {
  struct udmabuf_create create;
  int memfd, dmabuf_fd;

  memfd = memfd_create("udmabuf-parity", MFD_ALLOW_SEALING);
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

/* Import dmabuf_fd through dev_fd; fill maps[], return count (or -1). */
static int import_and_map(int dev_fd, int dmabuf_fd, struct udmabuf_dma_map *maps,
                          unsigned int cap) {
  struct udmabuf_attach attach;
  struct udmabuf_get_map *get_map;
  unsigned int count;

  memset(&attach, 0, sizeof(attach));
  attach.fd = dmabuf_fd;
  if (ioctl(dev_fd, UDMABUF_ATTACH, &attach)) {
    perror("UDMABUF_ATTACH");
    return -1;
  }
  count = attach.count;
  if (count > cap) {
    fprintf(stderr, "count %u exceeds cap %u\n", count, cap);
    return -1;
  }

  get_map = calloc(1, sizeof(*get_map) + (size_t)count * sizeof(*maps));
  if (!get_map) {
    perror("calloc");
    return -1;
  }
  get_map->fd = dmabuf_fd;
  get_map->count = count;
  if (ioctl(dev_fd, UDMABUF_GET_MAP, get_map)) {
    perror("UDMABUF_GET_MAP");
    free(get_map);
    return -1;
  }
  memcpy(maps, get_map->dma_arr, (size_t)count * sizeof(*maps));
  free(get_map);

  if (ioctl(dev_fd, UDMABUF_DETACH, &dmabuf_fd))
    perror("UDMABUF_DETACH (non-fatal)");

  return (int)count;
}

int main(void) {
  struct udmabuf_dma_map in_tree[256], module[256];
  int udmabuf_fd, import_fd, dmabuf_fd, ca, cb, mismatch = 0;

  udmabuf_fd = open("/dev/udmabuf", O_RDWR);
  if (udmabuf_fd < 0) {
    perror("open /dev/udmabuf");
    return 2;
  }
  import_fd = open("/dev/udmabuf_import", O_RDWR);
  if (import_fd < 0) {
    perror("open /dev/udmabuf_import");
    return 2;
  }

  dmabuf_fd = create_udmabuf(udmabuf_fd, BUF_SIZE);
  if (dmabuf_fd < 0)
    return 2;

  ca = import_and_map(udmabuf_fd, dmabuf_fd, in_tree, 256);
  cb = import_and_map(import_fd, dmabuf_fd, module, 256);
  if (ca < 0 || cb < 0)
    return 2;

  printf("in-tree /dev/udmabuf         : %d mapping(s)\n", ca);
  printf("module  /dev/udmabuf_import  : %d mapping(s)\n", cb);

  if (ca != cb) {
    printf("MISMATCH: count %d != %d\n", ca, cb);
    mismatch = 1;
  }
  for (int i = 0; i < ca && i < cb; i++) {
    printf("  [%d] in-tree 0x%llx/%llu   module 0x%llx/%llu\n", i,
           (unsigned long long)in_tree[i].dma_addr,
           (unsigned long long)in_tree[i].dma_len,
           (unsigned long long)module[i].dma_addr,
           (unsigned long long)module[i].dma_len);
    if (in_tree[i].dma_addr != module[i].dma_addr ||
        in_tree[i].dma_len != module[i].dma_len) {
      printf("  MISMATCH at index %d\n", i);
      mismatch = 1;
    }
  }

  printf("%s\n", mismatch ? "PARITY: FAIL" : "PARITY: OK");
  return mismatch ? 1 : 0;
}
