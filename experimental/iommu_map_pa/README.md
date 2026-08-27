# VFIO CUDA IOMMU Mapping Experiment

This directory contains an experimental proof of concept for direct
NVMe-to-GPU P2P DMA while the NVMe device is controlled by VFIO. It is not a
supported uPCIe API or a production-safe VFIO extension.

The helper accepts GPU device-physical addresses derived from the CUDA
`dma-buf`, inserts them into the IOMMU domain currently used by the target
VFIO device, and returns a handle for removing those mappings. A caller builds
NVMe PRPs from the resulting IOVA range.

uPCIe consumes it through `<upcie/dmamem_iommu_map_pa.h>`, which
wraps the helper as a `dmamem_registry` backend decorator so device memory
resolves through `dmamem_va_to_iova()` like everything else; see
`dmamem_from_cuda_iommu_map_pa()` and `dmamem_from_hip_iommu_map_pa()`. The raw
ioctl wrappers in `<upcie/experimental/iommu_map_pa.h>` remain available for
callers that want to place mappings themselves.

## Safety limitations

- The module calls `iommu_map()` on a live domain borrowed from
  `iommu_get_domain_for_dev()`. It does not own or pin that domain.
- The mappings bypass VFIO and iommufd IOVA accounting, locking, dirty
  tracking, and domain lifecycle management.
- The supplied `phys_lut` is assumed to contain addresses that are valid as
  `phys_addr_t` inputs to `iommu_map()` on the tested platform.
- The caller-selected IOVA range must not overlap mappings managed by VFIO or
  iommufd.
- Every experimental mapping must be removed before the VFIO container, HWPT,
  or device attachment is destroyed or changed. Through the dmamem decorator
  that means destroying the dmamem before closing the NVMe controller.
- The IOVA window the caller maps into is invisible to VFIO and iommufd, which
  will hand out the same addresses unless told not to. Under iommufd, keep the
  IOAS out of it with `dmamem_iommu_map_pa_reserve_window()`, after the target
  device is attached and before any host memory is mapped: the reservation
  constrains allocation from that call onwards, and mappings made before it
  keep whatever IOVAs the kernel gave them, window or not.
- Do not rebind the target device or alter its IOMMU domain while the helper
  file descriptor is open.

These constraints cannot be made safe merely by adding a lock inside this
module because VFIO and iommufd do not participate in that lock. The intended
long-term direction is an owner-integrated dma-buf mapping API such as
`IOMMU_IOAS_MAP_FILE` with CUDA dma-buf support.

## Install the module (DKMS)

The kernel module ships as a DKMS source package, so it rebuilds
automatically on kernel updates. Build the deb and install it:

```sh
cd experimental/iommu_map_pa
dpkg-buildpackage -us -uc -b
sudo apt install ../iommu-map-pa-dkms_*_all.deb
sudo modprobe iommu_map_pa
```

The package registers the module sources with DKMS and installs the ioctl
ABI system-wide as `<upcie/iommu_map_pa.h>`.

For quick hacking the plain out-of-tree build still works:

```sh
make -C experimental/iommu_map_pa/module
sudo insmod experimental/iommu_map_pa/module/iommu_map_pa.ko
```

## Exercise it

`tests/test_dmamem_cuda_iommu_map_pa_nvme_readwrite` drives the module through
uPCIe: an NVMe on vfio-cdev + iommufd, CUDA VRAM reached through the decorator,
and I/O verified in both directions. It builds with the rest of the tests when
CUDA is present.

> [!WARNING]
> It writes to namespace 1, LBA 0 of the selected NVMe controller. Run it only
> on a disposable test device whose contents may be destroyed.

Run it after binding the target NVMe to `vfio-pci`, reserving huge pages, and
loading this module and `dmabuf_import`:

```sh
sudo ./builddir/tests/test_dmamem_cuda_iommu_map_pa_nvme_readwrite \
    0000:01:00.0 /dev/vfio/devices/vfio0
```

Unload the module only after the test has exited and removed all mappings.
To remove the DKMS package, `sudo apt remove iommu-map-pa-dkms`.
