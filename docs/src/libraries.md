# Libraries

Every header is bundled by the umbrella `upcie.h`, or can be included on its own.
The descriptions below follow the bottom-up layering.

## Primitives

`bitfield.h`
: Macros and helpers for working with bitfields: extraction, masking, shifting,
  and printing of fields packed within integers or registers.

`barriers.h`
: Compiler and memory barriers for ordering loads and stores. Used by the MMIO
  and DMA paths to keep device-visible accesses in the intended order.

`debug.h`
: Conditional debug logging behind a single `UPCIE_DEBUG` macro. Enabled for
  debug builds and compiled out otherwise.

`mmio.h`
: Volatile 32-bit and 64-bit load and store helpers for MMIO access, suited to
  PCI BARs and device registers.

## PCI and VFIO

`pci.h`
: PCI device discovery, BDF parsing and formatting, and BAR mapping.

`vfioctl.h`
: Wraps the Linux VFIO ioctls with helpers and structs for managing containers,
  IOMMU groups, and devices, including DMA mapping into the IOMMU.

## Host memory

`hostmem.h`
: Top-level entry point for host memory management, delegating to the
  components below.

`hostmem_config.h`
: Shared sizing and granularity configuration, such as hugepage size and
  allocation granularity.

`hostmem_hugepage.h`
: Physically contiguous memory via Linux hugepages, with allocation and physical
  address resolution. Ideal for direct hardware access or P2P DMA.

`hostmem_heap.h`
: A simple heap allocator over a hugepage-backed region, with virtual-to-physical
  resolution per block.

`hostmem_dma.h`
: A malloc-like interface for allocating and freeing DMA-capable buffers.

## dma-buf

`dmabuf.h`
: Represents a dma-buf and the physical pages behind it, segments those pages
  into a LUT for raw-physical DMA, and pretty-prints the layout. The dma-buf may
  originate from host memory (memfd via udmabuf) or device memory such as CUDA.
  This header needs nothing beyond libc.

`experimental/dmabuf_import.h`
: **Experimental.** Resolves the DMA addresses behind a dma-buf
  (`dmabuf_import_attach`/`dmabuf_import_detach`), which is what populates the
  structure above. It imports the dma-buf through the out-of-tree
  `dmabuf_import` module, shipped in the `upcie-experimental` DKMS package
  (see `experimental/dmabuf_import`). When its UAPI header
  `<linux/dmabuf_import.h>` is absent these helpers compile as stubs returning
  `ENOTSUP` and `UPCIE_HAVE_DMABUF_IMPORT` is 0, so uPCIe still builds without
  it; the module must also be loaded at runtime for the import to succeed.

  The GPU memory helpers (`cudamem_heap.h`, `cudamem_mapping.h`,
  `hipmem_heap.h`, `hipmem_mapping.h`) all reach their physical addresses
  through this path, so they carry the same dependency.

## DMA memory

`dmamem.h`
: A region a device can read and write, plus the translation from a pointer
  into it to the address the device puts on the bus. One translator serves
  every flavour, and which applies follows the shape of the region: arithmetic
  from a base where it resolves as one contiguous range, a registry lookup
  where it resolves per granule.

`dmamem_registry.h`
: Resolves arbitrarily many registered regions through a granule-indexed
  table, so a lookup stays a single load however many are registered. What the
  device can address is the allocation a registration falls inside, held as a
  refcounted backing. Registration and removal are not thread-safe and the
  consumer serialises them; translation is lock-free.

`dmamem_heap.h`, `dmamem_hostmem.h`, `dmamem_cuda.h`, `dmamem_hip.h`,
`dmamem_memfd.h`, `dmamem_dmabuf.h`
: A suballocating heap over a dmamem, and the constructors for the flavours:
  hugepages, CUDA and HIP allocations, a memfd, and an existing dma-buf.

`dmamem_iommu_map_pa.h`
: **Experimental.** Decorates a `dmamem_registry` backend so its allocations are
  addressable with an enforcing IOMMU: the inner backend enumerates physical
  addresses as usual, the decorator inserts them into the target device's IOMMU
  domain, and the LUT is rewritten to hold the resulting IOVAs: a decorated LUT
  resolves to IOVAs, not to the physical addresses one otherwise holds.
  Translation and the submit path are unchanged, so a backend serves both
  `iommu=pt/off` and an enforcing IOMMU with one implementation; see
  `dmamem_from_cuda_iommu_map_pa()` and `dmamem_from_hip_iommu_map_pa()`. IOVAs
  come from a caller-reserved window the domain's owner must be kept out of:
  `dmamem_iommu_map_pa_reserve_window()` under iommufd, once the target device
  is attached, or a window above installed RAM by convention under a type1
  container. Carries the same DKMS dependency as `experimental/iommu_map_pa.h`.

## Experimental

Both out-of-tree modules ship in one binary package, `upcie-experimental-dkms`,
attached to each uPCIe release and carrying the uPCIe version. Installing it
unlocks every experimental path at once. The modules stay separate entities,
each with its own author, licence and README under
`/usr/src/upcie-experimental-<version>/`, so each can be discussed, and retired,
on its own once an upstream facility supersedes it.

### Module auto-load

Installing the package loads `dmabuf_import` and `iommu_map_pa`, and arranges
for them to load on boot via
`/usr/lib/modules-load.d/upcie-experimental.conf`. Both are misc devices with
no hardware to match on, so nothing else would autoload them, and the package
exists to make the experimental paths usable.

A load can fail, most often because Secure Boot is enabled and the DKMS-built
modules are unsigned with no enrolled MOK. That never fails the install; check
`dmesg` and `systemctl status systemd-modules-load` for what happened, and sign
the modules or disable Secure Boot to proceed.

To stop the modules loading on boot, mask the file:

```bash
sudo ln -s /dev/null /etc/modules-load.d/upcie-experimental.conf
```

They can still be loaded by hand with `modprobe dmabuf_import` and
`modprobe iommu_map_pa`.

`experimental/iommu_map_pa.h`
: **Experimental.** Maps an array of physical addresses into the IOMMU domain a
  VFIO-controlled device already uses (`iommu_map_pa_add`/`_del`),
  returning an IOVA base to address that memory through, e.g. from NVMe PRPs.
  Needs the out-of-tree `iommu_map_pa` module, shipped in the same
  `upcie-experimental` DKMS package (see `experimental/iommu_map_pa`). When its
  UAPI header
  `<linux/iommu_map_pa.h>` is absent these helpers compile as stubs returning
  `ENOTSUP` and `UPCIE_HAVE_IOMMU_MAP_PA` is 0, so uPCIe still builds without
  it; the module must also be loaded at runtime for the ioctls to succeed.

## Umbrella

`upcie.h`
: Includes all non-NVMe, non-CUDA components for convenient, all-in-one access.

## NVMe driver components

Enabled by defining `_UPCIE_WITH_NVME` before including the umbrella header.
These form a minimal user-space NVMe driver and are intentionally limited to
basic NVMe over PCIe.

`nvme_mmio.h`
: Accessors and structured views for the NVMe controller registers (CAP, VS, CC,
  CSTS, AQA, DB).

`nvme_controller.h`
: A `struct nvme_controller` wrapping BAR access, admin queue setup, and reset
  logic. The high-level entry point for interacting with a controller.

`nvme_controller_vfio.h`
: A VFIO-backed variant of the controller setup. Acquires the device through a
  VFIO container and group and maps its DMA buffers into the IOMMU, instead of
  the raw-physical sysfs path. A CUDA variant exists for GPU-direct DMA.

`nvme_qpair.h`
: A `struct nvme_qpair` for submission and completion queues, with allocation,
  doorbell management, and teardown.

`nvme_command.h`
: The NVMe command format and helpers for initializing common admin and I/O
  commands.

`nvme_request.h`
: A `struct nvme_request` tracking the lifecycle of a single command: metadata,
  payload, and completion.

`nvme_qid.h`
: An abstraction for queue identifiers, tracking queue type, index, and role.
