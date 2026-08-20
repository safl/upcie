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

## DMA memory

In contrast to dma-buf below, which is a Linux kernel framework these headers
merely wrap, `dmamem` is uPCIe's own abstraction. The kernel has no such
concept. It exists because DMA-able memory arrives from several unrelated
sources, hugepages, GPU runtimes, dma-buf exporters, BAR ranges, and because
the address the device needs is computed differently depending on which of them
it came from and on whether anything installed a mapping. `dmamem` is where
those differences are absorbed, so that code above it neither knows nor cares.

The name belongs to the same family as `hostmem`, `cudamem` and `hipmem`: each
is a *memory source*, and `dmamem` is the unified view across them. Do not
read it as a relative of the kernel's `dmabuf`, which is a neighbouring
spelling for a different kind of thing.

See {doc}`memory` for how these fit together and which combination applies to a
given setup.

`dmamem.h`
: A region of DMA-capable memory plus the rule for computing device addresses
  within it. Everything above it resolves addresses through `dmamem_va_to_iova()`
  and nothing above it needs to know what the memory is.

`dmamem_memfd.h`, `dmamem_dmabuf.h`, `dmamem_hostmem.h`, `dmamem_cuda.h`, `dmamem_hip.h`
: Constructors, one per combination of memory kind and kernel interface.

`dmamem_heap.h`
: A sub-allocator over a `dmamem`, handing out pieces of a region with
  virtual-to-device address resolution per allocation.

## Device memory

`cudamem_config.h`, `cudamem_heap.h`, `cudamem_dma.h`
: NVIDIA GPU memory: device page and allocation granularity, a heap over a
  `cuMemAlloc` reservation with its physical addresses enumerated, and a
  malloc-shaped interface over it.

`cudamem_mapping.h`
: A registry of externally-allocated CUDA buffers, resolving addresses for
  memory the caller allocated rather than the heap.

`hipmem_config.h`, `hipmem_heap.h`, `hipmem_dma.h`
: AMD GPU memory: device page and allocation granularity, a heap over a
  `hipMalloc` reservation with its physical addresses enumerated, and a
  malloc-shaped interface over it. The allocation granularity is the device
  page size, 4 KiB, rather than the 2 MiB typical on NVIDIA.

`hipmem_mapping.h`
: A registry of externally-allocated HIP buffers, resolving addresses for
  memory the caller allocated rather than the heap.

There is no `hostmem_mapping.h`. Registering caller-allocated memory is
supported for GPU memory but not for host memory, which is a gap rather than a
decision; see {doc}`memory`.

## dma-buf

dma-buf is a Linux kernel framework, not a uPCIe concept: it is the kernel's
mechanism for sharing buffers between drivers and across the user-space
boundary, where a buffer is represented by a file descriptor that an exporter
hands out and an importer attaches to. The authoritative documentation is the
kernel's own, under [Buffer Sharing and
Synchronization](https://docs.kernel.org/driver-api/dma-buf.html); nothing here
attempts to restate it.

The headers below are convenience libraries over the ioctl interfaces the
kernel exposes to user-space for that framework. They add no semantics of their
own beyond a C representation and error handling; where behaviour is
surprising, the kernel documentation is the place to look, not these.

`dmabuf.h`
: Represents a dma-buf and the physical pages behind it, segments those pages
  into a LUT for raw-physical DMA, and pretty-prints the layout. The dma-buf may
  originate from host memory (memfd via udmabuf) or device memory such as CUDA.
  This header needs nothing beyond libc.

`experimental/dmabuf_import.h`
: **Experimental.** A convenience library over the ioctls of an out-of-tree
  module rather than of the kernel proper. Resolves the DMA addresses behind a
  dma-buf (`dmabuf_import_attach`/`dmabuf_import_detach`), which is what
  populates the structure above. It imports the dma-buf through the out-of-tree
  `dmabuf_import` module, shipped as the `dmabuf-import` DKMS package (see
  `experimental/dmabuf_import`). When its UAPI header
  `<linux/dmabuf_import.h>` is absent these helpers compile as stubs returning
  `ENOTSUP` and `UPCIE_HAVE_DMABUF_IMPORT` is 0, so uPCIe still builds without
  it; the module must also be loaded at runtime for the import to succeed.

  The GPU memory helpers (`cudamem_heap.h`, `cudamem_mapping.h`,
  `hipmem_heap.h`, `hipmem_mapping.h`) all reach their physical addresses
  through this path, so they carry the same dependency.

## Experimental

`experimental/iommu_map_pa.h`
: **Experimental.** Likewise a convenience library over an out-of-tree module's
  ioctls. Maps an array of physical addresses into the IOMMU domain a
  VFIO-controlled device already uses (`iommu_map_pa_add`/`_del`),
  returning an IOVA base to address that memory through, e.g. from NVMe PRPs.
  Needs the out-of-tree `iommu-map-pa` DKMS package (see
  `experimental/iommu_map_pa`). When its UAPI header
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
