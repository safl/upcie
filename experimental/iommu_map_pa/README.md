# iommu-map-pa

An experimental helper that maps physical addresses into a device's live IOMMU
domain from userspace. It is not a supported uPCIe API or a production-safe VFIO
extension.

Given a PCI BDF, an array of physical addresses and a userspace-chosen IOVA
base, the module looks up the IOMMU domain the device currently uses and
installs `iova_base + i * page_size -> phys[i]` into it, returning a handle for
removing those mappings again. Nothing about it is specific to a memory source
or a device class: the addresses can come from anywhere the caller can resolve
them.

What it is for: direct NVMe-to-GPU P2P DMA while the NVMe is controlled by VFIO.
VFIO's own map API (`VFIO_IOMMU_MAP_DMA`) only accepts a pinnable host virtual
address, so GPU memory cannot be registered through it. The GPU's physical
addresses are already known, e.g. from a CUDA-exported `dma-buf` resolved
through `dmabuf_import`; the missing step is putting them into the domain the
NVMe actually translates through. This module is exactly that step, and the
included test builds NVMe PRPs from the resulting IOVA range.

> [!WARNING]
> The test writes to namespace 1, LBA 0 of the selected NVMe controller. Run it
> only on a disposable test device whose contents may be destroyed.

## Safety limitations

- Whoever can open `/dev/iommu_map_pa` can program a DMA-capable device to read
  or write **any** physical address they name. The node is registered `0600`
  root-only for that reason, and handing it to a group is handing out
  root-equivalence. See "Device node permissions" below.
- The target must already sit in a userspace-owned (unmanaged) IOMMU domain,
  i.e. be bound to `vfio-pci` or iommufd. Naming a driver-bound device is
  rejected rather than writing into the kernel's own DMA domain, where the
  IOVAs would collide with addresses the kernel later hands out.
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
cd experimental/iommu_map_pa
dpkg-buildpackage -us -uc -b
sudo apt install ../iommu-map-pa-dkms_*_all.deb
sudo modprobe iommu_map_pa
```

It replaces the older `upcie-iommu-map-dkms` package, which `apt` removes as
part of the install.

The package registers the module sources with DKMS and installs two headers:
the ioctl ABI as `<linux/iommu_map_pa.h>`, and the uPCIe userspace wrapper
(`upcie_iommu_map_pa_open/add/del`) as `<upcie/experimental/iommu_map_pa.h>`.
The wrapper lives under `experimental/` so that including it states the
out-of-tree dependency at the include line.

For quick hacking the plain out-of-tree build still works:

```sh
make -C experimental/iommu_map_pa/module
sudo insmod experimental/iommu_map_pa/module/iommu_map_pa.ko
```

## Device node permissions

`/dev/iommu_map_pa` is created `0600` root:root, so the test runs under `sudo`
by default. To run it as an unprivileged user instead, the package ships an
example `udev` rule that hands the node to a group:

```sh
sudo cp /usr/share/doc/iommu-map-pa-dkms/60-iommu-map-pa.rules.example \
        /etc/udev/rules.d/60-iommu-map-pa.rules
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=misc --action=add
```

The `trigger` matters: the rule is applied on a device `add` event, so an
already-loaded module keeps its old ownership until you trigger or reload it.
Group membership needs a fresh login session.

Read the warning above before doing this. The rule names the `vfio` group as an
example, which on many machines is broad precisely so that users can drive VFIO
devices; a dedicated group is the safer choice.

## Build the test

Configure the project normally, then explicitly build the experimental test:

```sh
meson setup builddir
meson compile -C builddir test_cudamem_iommu_map_pa_nvme_readwrite
```

The test binary is created at:

```text
builddir/experimental/iommu_map_pa/test_cudamem_iommu_map_pa_nvme_readwrite
```

Run it only after binding the target NVMe device to `vfio-pci` and setting up
the required huge pages:

```sh
sudo ./builddir/experimental/iommu_map_pa/test_cudamem_iommu_map_pa_nvme_readwrite 0000:02:00.0
```

Unload the module only after the test has exited and removed all mappings.
To remove the DKMS package, `sudo apt remove iommu-map-pa-dkms`.
