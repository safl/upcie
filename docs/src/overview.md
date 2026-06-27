# Overview

uPCIe began as an effort to verify behaviour in `vfio` and `vfio-pci`, around
the boundary between safe and unsafe user-space practices. It grew into a suite
of small, composable header-only libraries that target both the IOMMU-protected
path (`vfio-pci`) and the raw-physical path (`uio_pci_generic` with hugepages).

The libraries are header-only, idiomatic, zero-dependency beyond the Linux UAPI
headers, minimalistic, low-coupling, and C11. The umbrella header `upcie.h`
bundles all non-NVMe, non-CUDA components. Include it for convenience, or
include individual headers to control exactly which toolchain and UAPI headers
are pulled in.

## Layers

The headers build bottom-up:

1. Primitives: bit manipulation, memory barriers, MMIO accessors, debug logging.
2. PCI: device enumeration via sysfs, BDF parsing, BAR mapping.
3. VFIO: container, group, and device management with IOMMU DMA mapping.
4. Host memory: hugepage allocation, virt-to-phys resolution, a heap allocator,
   and a DMA malloc interface.
5. dma-buf: share a dma-buf across devices and resolve its physical pages.
6. NVMe (optional): a minimal user-space NVMe driver built on the above.

## Errors and ownership

Functions return negative `errno` values on error and `NULL` on allocation
failure. The context or handle is always the first parameter. Array parameters
use `elem_count` and `elem_size` naming.

## Companion tools

uPCIe is libraries only. The command-line tooling that grew alongside it lives
in its own repositories, distributed as standalone PyPI packages:

- [`devbind`](https://github.com/xnvme/devbind): list PCIe devices and bind or
  unbind their drivers.
- [`hugepages`](https://github.com/xnvme/hugepages): inspect, reserve, and mount
  hugepages.
- [`iommu`](https://github.com/safl/iommu): inspect the IOMMU mode and switch it
  by rewriting the kernel command line, applied on the next boot.

The [nosi](https://github.com/safl/nosi) images used for development and testing
ship with them pre-installed.
