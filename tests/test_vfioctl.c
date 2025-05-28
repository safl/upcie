#include <vfioctl.h>

int main(void) {
  struct vfio_container container = {0};
  struct vfio_group group = {0};
  struct vfio_device dev = {0};
  int err, api_version;

  err = vfio_container_open(&container);
  if (err) {
    perror("vfio_open_container");
    return 1;
  }

  err = vfio_get_api_version(&container, &api_version);
  if (api_version != VFIO_API_VERSION) {
    fprintf(stderr, "Unexpected VFIO API version: %d\n", api_version);
    vfio_container_close(&container);
    return 1;
  }

  if (!vfio_check_extension(&container, VFIO_TYPE1_IOMMU)) {
    fprintf(stderr, "VFIO_TYPE1_IOMMU not supported\n");
    vfio_container_close(&container);
    return 1;
  }

  printf("VFIO API version: %d\n", api_version);
  printf("VFIO_TYPE1_IOMMU supported\n");

  err = vfio_group_open(14, &group);
  if (err) {
    perror("vfio_open_group");
    vfio_container_close(&container);
    return 1;
  }

  err = vfio_group_get_status(&group, &group.status);
  if (err < 0) {
    perror("vfio_group_get_status");
    goto exit;
  }

  if (!(group.status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
    fprintf(stderr, "Group not viable\n");
    goto exit;
  }

  printf("Group is viable\n");

  err = vfio_group_set_container(&group, &container);
  if (err < 0) {
    perror("vfio_group_set_container");
    goto exit;
  }

  err = vfio_set_iommu(&container, VFIO_TYPE1_IOMMU);
  if (err < 0) {
    perror("vfio_set_iommu");
    goto exit;
  }

  printf("VFIO setup complete\n");

exit:
  vfio_group_close(&group);
  vfio_container_close(&container);
  return 0;
}
