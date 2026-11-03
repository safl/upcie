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
make rust             # cargo fmt --check, clippy with warnings as errors, unit tests
make rust-aarch64     # type-check for aarch64, whose DMA barriers differ
make rust-artifacts   # the NVMe example, static with musl, into /tmp/artifacts
```

The unit tests need no device. The guest tests in `tests/test_nvme_rs.py` run
the NVMe example over both drivers, beside the C example. `make deploy` builds
it first, statically for `x86_64-unknown-linux-musl`, so it needs Rust 1.85 or
later with that target added (`rustup target add x86_64-unknown-linux-musl`);
`RUST_TARGET` picks another. CI builds it in its `rust` job.
