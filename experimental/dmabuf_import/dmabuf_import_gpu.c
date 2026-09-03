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
#include "dmabuf_import_placement.h"

#include <cuda.h>

/* cuCtxCreate gained a create-params argument (cuCtxCreate_v4) in CUDA 12.5;
 * wrap both signatures so the example builds across toolkit versions. */
#if CUDA_VERSION >= 12050
#define CU_CTX_CREATE(pctx, flags, dev) cuCtxCreate((pctx), NULL, (flags), (dev))
#else
#define CU_CTX_CREATE(pctx, flags, dev) cuCtxCreate((pctx), (flags), (dev))
#endif

struct gpu_dmabuf_info {
  CUmemGenericAllocationHandle dmabuf_fd;
  CUdeviceptr vaddr;
  CUcontext ctx;
};

int create_nvidia_dmabuf_fd(struct gpu_dmabuf_info *gdi, size_t buf_size) {
  CUmemGenericAllocationHandle dmabuf_fd;
  CUdeviceptr vaddr;
  CUdevice dev;
  CUcontext ctx;
  CUresult err;
  int gpu_id = 0;

  err = cuInit(0);
  if (err) {
    printf("cuInit failed: %d\n", err);
    return err;
  }

  err = cuDeviceGet(&dev, gpu_id);
  if (err) {
    printf("cuDeviceGet failed: %d\n", err);
    return err;
  }

  err = CU_CTX_CREATE(&ctx, 0, dev);
  if (err) {
    printf("cuCtxCreate failed: %d\n", err);
    return err;
  }

  err = cuMemAlloc(&vaddr, buf_size);
  if (err) {
    printf("cuMemAlloc failed: %d\n", err);
    return err;
  }

  err = cuMemGetHandleForAddressRange(&dmabuf_fd, vaddr, buf_size,
                                      CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD, 0);
  if (err) {
    printf("cuMemGetHandleForAddressRange failed: %d\n", err);
    return err;
  }

  gdi->dmabuf_fd = dmabuf_fd;
  gdi->vaddr = vaddr;
  gdi->ctx = ctx;

  return 0;
}

void destroy_nvidia_dmabuf_fd(struct gpu_dmabuf_info *gdi) {
  cuCtxDestroy(gdi->ctx);
  cuMemFree(gdi->vaddr);
}

int main(int argc, char *argv[]) {
  struct dmabuf_import_attach *attach;
  struct dmabuf_import_get_map *map;
  struct gpu_dmabuf_info gpu_dmabuf_info;
  int import_fd, dmabuf_fd, err;
  size_t buf_size = 8 * 65536; // 8 GPU pages
  long map_size;

  /* The GPU dma-buf comes from CUDA, not UDMABUF_CREATE, so stock /dev/udmabuf
   * is not used here. The out-of-tree module serves the DMABUF_IMPORT_ATTACH /
   * GET_MAP import ioctls on its own device. */
  import_fd = open(DMABUF_IMPORT_DEVPATH, O_RDWR);
  if (import_fd < 0) {
    err = errno;
    printf("Failed to open dmabuf_import dev, errno: %d\n", err);
    return err;
  }

  err = create_nvidia_dmabuf_fd(&gpu_dmabuf_info, buf_size);
  if (err) {
    return err;
  }

  dmabuf_fd = (int)gpu_dmabuf_info.dmabuf_fd;

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

  dmabuf_import_placement(import_fd, dmabuf_fd, argc > 1 ? argv[1] : NULL, "placement:");


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
    printf("IOCTL DMABUF_IMPORT_GET_MAP failed, %d\n", err);
    return err;
  }

  for (int i = 0; i < map->count; i++) {
    printf("addr %d: 0x%llx\n", i, map->dma_arr[i].dma_addr);
    printf("len %d: %lld\n", i, map->dma_arr[i].dma_len);
  }

  err = ioctl(import_fd, DMABUF_IMPORT_GET_MAP, map);
  if (err) {
    err = errno;
    printf("IOCTL DMABUF_IMPORT_GET_MAP failed, errno: %d\n", err);
    return err;
  }

  destroy_nvidia_dmabuf_fd(&gpu_dmabuf_info);

  close(import_fd);
  free(attach);
  free(map);

  return err;
}