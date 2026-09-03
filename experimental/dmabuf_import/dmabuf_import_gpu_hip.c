#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "module/dmabuf_import.h"

#define __HIP_PLATFORM_AMD__
#include <hip/hip_runtime.h>

/* The AMD counterpart to dmabuf_import_gpu.c. It exists because the two
 * runtimes fail differently: amdgpu refuses to map its VRAM to an importer
 * that has not declared peer-to-peer support, so this is what shows whether
 * the module attaches as one. */

struct gpu_dmabuf_info {
  int dmabuf_fd;
  void *vaddr;
};

int create_amd_dmabuf_fd(struct gpu_dmabuf_info *gdi, size_t buf_size) {
  int dmabuf_fd = -1;
  void *vaddr = NULL;
  int err;

  err = hipInit(0);
  if (err) {
    printf("hipInit failed: %d\n", err);
    return err;
  }

  err = hipSetDevice(0);
  if (err) {
    printf("hipSetDevice failed: %d\n", err);
    return err;
  }

  err = hipMalloc(&vaddr, buf_size);
  if (err) {
    printf("hipMalloc failed: %d\n", err);
    return err;
  }

  err = hipMemGetHandleForAddressRange(&dmabuf_fd, (hipDeviceptr_t)vaddr, buf_size,
                                       hipMemRangeHandleTypeDmaBufFd, 0);
  if (err) {
    printf("hipMemGetHandleForAddressRange failed: %d\n", err);
    return err;
  }

  gdi->dmabuf_fd = dmabuf_fd;
  gdi->vaddr = vaddr;

  return 0;
}

void destroy_amd_dmabuf_fd(struct gpu_dmabuf_info *gdi) {
  close(gdi->dmabuf_fd);
  hipFree(gdi->vaddr);
}

int main(int argc, char *argv[]) {
  struct dmabuf_import_attach *attach;
  struct dmabuf_import_get_map *map;
  struct gpu_dmabuf_info gpu_dmabuf_info;
  /* AMD's dma-buf export wants the range 2 MiB aligned and sized. */
  size_t buf_size = 2 * 1024 * 1024;
  int import_fd, dmabuf_fd, err;
  long map_size;

  (void)argc;
  (void)argv;

  import_fd = open(DMABUF_IMPORT_DEVPATH, O_RDWR);
  if (import_fd < 0) {
    err = errno;
    printf("Failed to open dmabuf_import dev, errno: %d\n", err);
    return err;
  }

  err = create_amd_dmabuf_fd(&gpu_dmabuf_info, buf_size);
  if (err) {
    return err;
  }

  dmabuf_fd = gpu_dmabuf_info.dmabuf_fd;

  printf("DMABUF FD: %d\n", dmabuf_fd);

  attach = malloc(sizeof(struct dmabuf_import_attach));
  if (!attach) {
    err = errno;
    printf("Failed to alloc attach struct, errno: %d\n", err);
    return err;
  }
  memset(attach, 0, sizeof(*attach));
  attach->fd = dmabuf_fd;

  err = ioctl(import_fd, DMABUF_IMPORT_ATTACH, attach);
  if (err) {
    err = errno;
    printf("IOCTL DMABUF_IMPORT_ATTACH failed, errno: %d\n", err);
    return err;
  }

  printf("dma-buf contains %u addresses\n", attach->count);

  map_size = attach->count * sizeof(struct dmabuf_import_dma_map);

  map = malloc(sizeof(struct dmabuf_import_get_map) + map_size);
  if (!map) {
    err = errno;
    printf("Failed to alloc map struct, errno: %d\n", err);
    return err;
  }
  memset(map, 0, sizeof(*map));

  map->fd = dmabuf_fd;
  map->count = attach->count;

  err = ioctl(import_fd, DMABUF_IMPORT_GET_MAP, map);
  if (err) {
    err = errno;
    printf("IOCTL DMABUF_IMPORT_GET_MAP failed, errno: %d\n", err);
    return err;
  }

  for (int i = 0; i < map->count; i++) {
    printf("addr %d: 0x%llx\n", i, map->dma_arr[i].dma_addr);
    printf("len %d: %lld\n", i, map->dma_arr[i].dma_len);
  }

  /* Detached before the device memory goes: the module holds the attachment
   * until told otherwise, and freeing VRAM under a live import is what
   * move_notify is there to report. */
  err = ioctl(import_fd, DMABUF_IMPORT_DETACH, &dmabuf_fd);
  if (err) {
    err = errno;
    printf("IOCTL DMABUF_IMPORT_DETACH failed, errno: %d\n", err);
    return err;
  }

  close(import_fd);
  destroy_amd_dmabuf_fd(&gpu_dmabuf_info);

  free(attach);
  free(map);

  return err;
}
