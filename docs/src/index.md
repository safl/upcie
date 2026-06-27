# User-space PCIe libraries

```{only} html
[![verify](https://github.com/safl/upcie/actions/workflows/verify.yml/badge.svg)](https://github.com/safl/upcie/actions/workflows/verify.yml)
[![docs](https://github.com/safl/upcie/actions/workflows/docs.yml/badge.svg)](https://safl.dk/upcie)
[![license](https://img.shields.io/badge/license-BSD--3--Clause-blue.svg)](https://github.com/safl/upcie/blob/main/LICENSE)
```

uPCIe is a set of header-only C libraries for user-space PCIe device interaction
on Linux. It provides hugepage-backed DMA, MMIO accessors, PCI discovery and BAR
mapping, VFIO wrappers, dma-buf helpers, and a minimalist NVMe driver, with no
dependencies beyond the Linux UAPI headers.

It started as a way to explore the boundary between safe and unsafe user-space
PCIe practices. It targets both the IOMMU-protected path (`vfio-pci`) and the
raw-physical path (`uio_pci_generic` with hugepages). The companion tooling
lives in its own repositories: [devbind](https://github.com/xnvme/devbind),
[hugepages](https://github.com/xnvme/hugepages), and
[iommu](https://github.com/safl/iommu).

## Design goals

- **Header-only**
- **Idiomatic**, with a consistent error and ownership model
- **Zero-dependency**, beyond the Linux UAPI headers
- **Minimalistic**
- **Low-coupling**
- **C11**

## Topics of interest

- Mapping PCIe BAR spaces via `vfio-pci` and `uio_pci_generic`
- IOMMU-protected memory for safe DMA
- Hugepages for raw-physical DMA with `uio_pci_generic`
- PCIe driver binding and unbinding
- Interrupts and how to handle them

```{toctree}
:maxdepth: 2
:caption: Get started

overview
quickstart
build-and-test
```

```{toctree}
:maxdepth: 2
:caption: Reference

libraries
api
```
