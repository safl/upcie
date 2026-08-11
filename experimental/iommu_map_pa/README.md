# VFIO CUDA IOMMU Mapping Experiment

This directory contains an experimental proof of concept for direct
NVMe-to-GPU P2P DMA while the NVMe device is controlled by VFIO. It is not a
supported uPCIe API or a production-safe VFIO extension.

> [!WARNING]
> The test writes to namespace 1, LBA 0 of the selected NVMe controller. Run it
> only on a disposable test device whose contents may be destroyed.

The helper accepts GPU device-physical addresses derived from the CUDA
`dma-buf`, inserts them into the IOMMU domain currently used by the target
VFIO device, and returns a handle for removing those mappings. The test builds
NVMe PRPs from the resulting IOVA range.

## Safety limitations

- Whoever can open the device node can program a DMA-capable device to read or
  write **any** physical address they name, so the node is registered `0600`
  root-only and handing it to a group is handing out root-equivalence.
- The target must already sit in a userspace-owned (unmanaged) IOMMU domain,
  i.e. be bound to `vfio-pci` or iommufd. Naming a driver-bound device is
  rejected rather than writing into the kernel's own DMA domain.
- The module calls `iommu_map()` on a live domain borrowed from
  `iommu_get_domain_for_dev()`. It does not own or pin that domain.
- The mappings bypass VFIO and iommufd IOVA accounting, locking, dirty
  tracking, and domain lifecycle management.
- The supplied `phys_lut` is assumed to contain addresses that are valid as
  `phys_addr_t` inputs to `iommu_map()` on the tested platform.
- The caller-selected IOVA range must not overlap mappings managed by VFIO or
  iommufd.
- Every experimental mapping must be removed before the VFIO container, HWPT,
  or device attachment is destroyed or changed.
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
cd experimental/vfio_cuda_iommu_map
dpkg-buildpackage -us -uc -b
sudo apt install ../upcie-iommu-map-dkms_*_all.deb
sudo modprobe upcie_iommu_map
```

The package registers the module sources with DKMS and installs the ioctl
ABI system-wide as `<upcie/upcie_iommu_map.h>`.

For quick hacking the plain out-of-tree build still works:

```sh
make -C experimental/vfio_cuda_iommu_map/module
sudo insmod experimental/vfio_cuda_iommu_map/module/upcie_iommu_map.ko
```

## Build the test

Configure the project normally, then explicitly build the experimental test:

```sh
meson setup builddir
meson compile -C builddir test_cudamem_iommu_map_nvme_readwrite
```

The test binary is created at:

```text
builddir/experimental/vfio_cuda_iommu_map/test_cudamem_iommu_map_nvme_readwrite
```

Run it only after binding the target NVMe device to `vfio-pci` and setting up
the required huge pages:

```sh
sudo ./builddir/experimental/vfio_cuda_iommu_map/test_cudamem_iommu_map_nvme_readwrite 0000:02:00.0
```

Unload the module only after the test has exited and removed all mappings.
To remove the DKMS package, `sudo apt remove upcie-iommu-map-dkms`.
