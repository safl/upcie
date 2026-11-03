// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

//! Ordering between memory a device reaches by DMA and the registers that tell it to look
//!
//! What `dma_wmb()` and `dma_rmb()` of `<upcie/barriers.h>` do for C, and what Linux calls them.
//! Rust's own fences order memory only among CPUs: on aarch64 they reach the inner shareable
//! domain, and a device is in the outer one. [`crate::dmamem::DmaMem`] uses these around its
//! accesses, so they matter only to code writing DMA memory by other means.

use std::sync::atomic::{Ordering, compiler_fence};

#[cfg(not(any(target_arch = "x86_64", target_arch = "aarch64")))]
compile_error!("uPCIe's DMA barriers are defined for x86_64 and aarch64 only");

/// Order writes to DMA memory before what follows, such as the doorbell telling the device
#[inline]
pub fn dma_wmb() {
    // SAFETY: a barrier, touching no memory and no registers
    #[cfg(target_arch = "aarch64")]
    unsafe {
        std::arch::asm!("dmb oshst", options(nostack, preserves_flags))
    };
    // x86_64 orders stores to write-back memory with each other and with uncached MMIO stores
    compiler_fence(Ordering::SeqCst);
}

/// Order reads of DMA memory after what came before, such as the read seeing a completion's phase
#[inline]
pub fn dma_rmb() {
    // SAFETY: a barrier, touching no memory and no registers
    #[cfg(target_arch = "aarch64")]
    unsafe {
        std::arch::asm!("dmb oshld", options(nostack, preserves_flags))
    };
    // x86_64 does not reorder loads with each other
    compiler_fence(Ordering::SeqCst);
}
