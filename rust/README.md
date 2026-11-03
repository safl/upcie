# upcie

User-space PCIe on Linux in Rust: PCI functions through sysfs, devices through
their VFIO character device, I/O address spaces through iommufd, mapped register
spaces, and memory a device reaches by DMA, on both the IOMMU path (`vfio-pci`)
and the raw-physical path (`uio_pci_generic` with hugepages). No dependencies.

The Rust side of [uPCIe](https://github.com/safl/upcie); `examples/nvme.rs` is a
small NVMe driver on it. See `docs/src/rust.md` in the repository.

## Status

A work in progress, exploring what a Rust version of uPCIe looks like. It is
enough to drive one device from user space on x86_64, which the NVMe example
does in uPCIe's CI over both paths. Interrupts, reset, detach, `map_slice`, and
the fallback mapping DMA memory by address on kernels before 6.13, are in the
API but have not been run against a device.

Not there yet in Rust, though all of it is available today in uPCIe's C
implementation, header-only, at <https://github.com/safl/upcie>:

- a heap to sub-allocate DMA memory; queues and buffers are laid out by hand
- checking a mapping's IOVA against the ranges the IOMMU allows
- dma-buf, so no peer-to-peer with GPUs or other devices
- VFIO type1, for setups without the VFIO character device
- an NVMe library; the example is an example
- aarch64 beyond type-checking

The C headers also cover GPU memory through CUDA and HIP, and an NVMe control
plane sharing one controller between processes. Expect the Rust API to change
between 0.x releases.
