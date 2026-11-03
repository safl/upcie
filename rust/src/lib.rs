// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

//! uPCIe for Rust: user-space PCIe on Linux, with no dependencies
//!
//! The same ground the C headers cover, in Rust's terms: a function found in sysfs
//! ([`pci`]), its registers mapped and accessed ([`mmio`]), the device opened through its VFIO
//! character device ([`vfio`]) and given an I/O address space through iommufd ([`iommufd`]),
//! and memory the device reaches by DMA ([`dmamem`]). Without an IOMMU, the raw-physical path
//! maps BARs through sysfs and gives the device hugepage memory by its physical address.
//!
//! `examples/nvme.rs` puts it together as a small NVMe driver over either path.

pub mod barrier;
pub mod dmamem;
pub mod iommufd;
pub mod mmio;
pub mod pci;
pub mod vfio;

mod sys;

pub use sys::page_size;
