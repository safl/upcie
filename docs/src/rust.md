# Rust

The crate in `rust/` brings uPCIe to Rust: sysfs PCI, MMIO, VFIO character
devices, iommufd, DMA memory and DMA barriers, in Rust's terms rather than as
bindings to the C headers. The heaps, dma-buf helpers and NVMe driver have no
counterpart yet; `examples/nvme.rs` is an example, not a library. It keeps the
C headers' goal: no dependencies beyond the Linux UAPI, here not even a crate. The C library
`std` already links provides the few functions it calls.

| Module    | What it does                                                        | C counterpart                     |
| --------- | ------------------------------------------------------------------- | --------------------------------- |
| `pci`     | addresses, identity, bound driver, config space, BARs through sysfs | `pci.h`                           |
| `mmio`    | a mapped register space, volatile and bounds-checked                | `mmio.h`                          |
| `vfio`    | a device through its VFIO character device: regions, IRQs, reset    | `vfio_cdev.h`, `vfioctl.h`        |
| `iommufd` | I/O address spaces, mapping by address or by file                   | `iommufd.h`                       |
| `dmamem`  | DMA memory: memfd in an IOAS, or a hugepage by physical address     | `dmamem_memfd.h`, `hostmem_*.h`   |
| `barrier` | ordering DMA memory against the device's doorbells                  | `barriers.h`                      |

As in C, both paths are covered: `vfio-pci` with iommufd, and `uio_pci_generic`
with hugepages by their physical addresses, which needs root.

## Parity with the C headers

What each C header has in Rust, what it is to have, and what holds it. A
row is done when its Rust counterpart exists and the test named holds it;
"port" is to be written; "decide" waits on a question below; "not planned"
gives its reason. Read from main at 0.11.1, by what each function does
rather than its name.

| C | Rust today | Plan | Held by |
| --- | --- | --- | --- |
| `pci.h`: `pci_addr_*` | `pci::Bdf`, parsed and printed | done | unit tests |
| `pci.h`: `pci_func_open()`, `pci_bar_size()`, `pci_bar_map()` | `pci::Function::open()`, `bar_size()`, `map_bar()` | done | the NVMe example over uio |
| `pci.h`: `pci_scan()` | none | port: the functions sysfs lists | guest test |
| `pci.h`: `pci_bar_largest_size()` | none | port | guest test |
| `pci.h`: `pci_func_pr()`, `pci_bar_pr()` | none | port as `Debug` | unit test |
| `vfio_cdev.h` | `vfio::Device::open()` | done | the NVMe example over vfio-pci |
| `vfioctl.h`: device info, regions, IRQs, `vfio_device_reset()`, `vfio_map_region()` | `vfio::Device`: `info()`, `region()`, `map_region()`, `irq_count()`, `enable_irqs()`, `reset()` | done; IRQs and reset not yet run against a device | the NVMe example; a guest test for IRQs and reset |
| `vfioctl.h`: `vfio_device_bar_export_dmabuf()` | none | port, first | guest test: export a BAR, map it into an IOAS |
| `vfioctl.h`: `vfio_device_get_pci_hot_reset_info()`, `vfio_device_pci_hot_reset()` | none | port | guest test |
| `vfioctl.h`: `vfio_device_get_iommu_group_id()` | none | port | guest test |
| `vfioctl.h`: type1, containers, groups, `vfio_iommu_map_dma()` | none | decide (Q3) | |
| `iommufd.h`: open, IOAS, bind, attach, detach, map, map a file, unmap, destroy | `iommufd::Iommufd`, `Ioas`, `Mapping`, unmapped and destroyed on drop | done | the NVMe example over vfio-pci; `map_slice()` not yet run |
| `iommufd.h`: `iommufd_ioas_iova_ranges()`, `iommufd_ioas_allow_iovas()` | none | port, second: a mapping checked against the ranges | guest test |
| `mmio.h` | `mmio::Mmio` | done | the NVMe example |
| `barriers.h`: `dma_rmb()` | `barrier::dma_rmb()`, and `dma_wmb()`, which C lacks | done | the NVMe example, through `DmaMem::read()` and `write()` |
| `barriers.h`: `mb()`, `rmb()`, `wmb()`, `barrier()`, `cpu_relax()` | none | not planned: `std::sync::atomic::fence` and `std::hint::spin_loop` are Rust's | |
| `bitfield.h` | none | not planned: Rust's integer methods | |
| `debug.h` | none | not planned | |
| `dmamem_memfd.h` | `dmamem::DmaMem::in_ioas()` | done | the NVMe example over vfio-pci |
| `hostmem_hugepage.h`: alloc and free | `DmaMem::hugepage()`, 2 MiB only | done | the NVMe example over uio |
| `hostmem_hugepage.h`: `hostmem_hugepage_import()` | none | port | guest test |
| `hostmem_config.h`: the hugetlbfs mount and page size | none, 2 MiB fixed | port: 1 GiB pages | unit test |
| `hostmem.h`: `hostmem_pagemap_virt_to_phys()` | `dmamem::physical()` | done | the NVMe example over uio, through `DmaMem::hugepage()` |
| `dmamem.h`: `dmamem_va_to_iova()`, `dmamem_offset_to_iova()` | `DmaMem::iova()` plus an offset, one buffer at a time | decide (Q1) | |
| `dmamem_heap.h`, `hostmem_heap.h`, `hostmem_dma.h` | none | decide (Q1) | |
| `dmamem_registry.h` | none | decide (Q1) | |
| `dmamem_hostmem.h`: sharing DMA memory between processes | none | port, with the NVMe control plane | guest test |
| `dmamem_hostmem.h`: `dmamem_from_hostmem_type1()` | none | decide (Q3) | |
| `dmabuf.h`, `dmamem_dmabuf.h` | none | port, first: map a dma-buf into an IOAS, describe its pages | guest test, with a udmabuf |
| `cudamem_*.h`, `hipmem_*.h`, `dmamem_cuda.h`, `dmamem_hip.h`, `upcie_cuda.h`, `upcie_hip.h` | none | decide (Q2) | |
| `experimental/dmabuf_import.h`, `experimental/iommu_map_pa.h` | none | not planned while the modules are experimental | |
| `nvme/` | `examples/nvme.rs`, an example | port after Q1, a library of its own, its own issue | the guest tests the C NVMe code has |
| aarch64 | type-checked | port: run where CI has an aarch64 machine | CI |

Open questions, whose answers shape the crate's API:

- **Q1, sub-allocating DMA memory.** The C heaps carve buffers out of one
  mapping and the registry finds the IOVA of an address across several.
  Should Rust do the same, or hand out buffers that own their slice of a
  mapping, borrowed from it, so that the borrow checker keeps a buffer
  from outliving its mapping?
- **Q2, CUDA and HIP memory.** Port it behind cargo features, keeping the
  crate free of dependencies by default, or leave it to C?
- **Q3, VFIO type1.** Carry the container and group path for kernels and
  setups without the VFIO character device, or require the cdev?

The order: the two rows marked first, then the IOVA ranges, then Q1 and
what waits on it, then the rest.

## The NVMe example

`examples/nvme.rs` is a small NVMe driver on the crate, the counterpart of
`example/upcie_nvme_driver.c`. It picks its path from the driver the function is
bound to, brings the controller up as the NVMe Base Specification lays out,
identifies it and namespace 1, sets the number of I/O queues, creates one queue
pair, reads the first block, and tears everything down again:

```bash
cd rust
cargo run --release --example nvme -- 0000:01:00.0
```

## Building and testing

```bash
make rust             # cargo fmt --check, clippy with warnings as errors, unit tests, package
make rust-aarch64     # type-check for aarch64, whose DMA barriers differ
make rust-artifacts   # the NVMe example, static with musl, into /tmp/artifacts
```

The unit tests need no device. The guest tests in `tests/test_nvme_rs.py` run
the NVMe example over both drivers, beside the C example. `make deploy` builds
it first, statically for `x86_64-unknown-linux-musl`, so it needs Rust 1.85 or
later with that target added (`rustup target add x86_64-unknown-linux-musl`);
`RUST_TARGET` picks another. CI builds it in its `rust` job.

## Releases

The crate carries the uPCIe version and is released with it as `upcie` on
crates.io. `make bump` sets its version with the rest, `make rust` fails when
they differ, and a `v*` tag publishes it once CI has passed, through crates.io's
trusted publishing, so the repository holds no token.
