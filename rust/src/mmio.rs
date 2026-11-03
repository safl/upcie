// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

//! A mapped register space, read and written with volatile accesses
//!
//! What `<upcie/mmio.h>` does for C: a BAR mapped into the process, from sysfs or from VFIO, and
//! accessed register by register. Accesses are bounds-checked and naturally aligned.

use std::fmt;
use std::io;
use std::marker::PhantomData;
use std::os::fd::{AsRawFd, BorrowedFd};
use std::ptr::NonNull;

use crate::sys;

/// A mapping of a device's register space, unmapped on drop
///
/// `'d` is what the mapping depends on: a VFIO device, whose regions the kernel unmaps when it is
/// closed, or nothing, `'static`, for a BAR mapped through sysfs.
pub struct Mmio<'d> {
    base: sys::Base,
    len: usize,
    _device: PhantomData<&'d ()>,
}

impl Mmio<'_> {
    /// Map `len` bytes of `fd` from `offset`, shared, for reading and writing
    ///
    /// The mapping outlives `fd`. Where the kernel takes it away with something else, as VFIO
    /// does with a device's regions, tie it to that, as [`crate::vfio::Device::map_region`] does.
    pub fn map(fd: BorrowedFd<'_>, offset: u64, len: usize) -> io::Result<Mmio<'static>> {
        if len == 0 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "an empty mapping",
            ));
        }
        // SAFETY: a fresh mapping of a descriptor the caller holds open
        let ptr = unsafe {
            sys::mmap(
                std::ptr::null_mut(),
                len,
                sys::PROT_READ | sys::PROT_WRITE,
                sys::MAP_SHARED,
                fd.as_raw_fd(),
                offset as i64,
            )
        };
        if ptr == sys::MAP_FAILED {
            return Err(io::Error::last_os_error());
        }
        Ok(Mmio {
            base: sys::Base(NonNull::new(ptr.cast()).expect("mmap returned null")),
            len,
            _device: PhantomData,
        })
    }

    /// The mapping's length in bytes
    pub fn len(&self) -> usize {
        self.len
    }

    /// Whether the mapping is empty, which `map` never makes
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    fn at<T>(&self, off: usize) -> *mut T {
        let size = std::mem::size_of::<T>();
        assert!(
            off % size == 0 && off.checked_add(size).is_some_and(|end| end <= self.len),
            "register access of {size} bytes at {off:#x} outside a {:#x}-byte mapping, or unaligned",
            self.len
        );
        // SAFETY: checked to lie inside the mapping
        unsafe { self.base.0.as_ptr().add(off).cast() }
    }

    /// Read the 32-bit register at `off`
    pub fn read_u32(&self, off: usize) -> u32 {
        // SAFETY: inside the mapping and aligned, as `at` checks
        unsafe { self.at::<u32>(off).read_volatile() }
    }

    /// Write the 32-bit register at `off`
    pub fn write_u32(&self, off: usize, val: u32) {
        // SAFETY: inside the mapping and aligned, as `at` checks
        unsafe { self.at::<u32>(off).write_volatile(val) }
    }

    /// Read the 64-bit register at `off`
    pub fn read_u64(&self, off: usize) -> u64 {
        // SAFETY: inside the mapping and aligned, as `at` checks
        unsafe { self.at::<u64>(off).read_volatile() }
    }

    /// Write the 64-bit register at `off`, as two 32-bit writes, low half first
    ///
    /// Devices differ in whether they take a 64-bit store whole, and two halves are what every
    /// one of them accepts; NVMe's `ASQ` and `ACQ` are written so by Linux as well.
    pub fn write_u64(&self, off: usize, val: u64) {
        self.write_u32(off, val as u32);
        self.write_u32(off + 4, (val >> 32) as u32);
    }
}

impl fmt::Debug for Mmio<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Mmio").field("len", &self.len).finish()
    }
}

impl Drop for Mmio<'_> {
    fn drop(&mut self) {
        // SAFETY: the mapping `map` made, unmapped once
        unsafe { sys::munmap(self.base.0.as_ptr().cast(), self.len) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::fd::{AsFd, FromRawFd, OwnedFd};

    fn memfd(len: u64) -> OwnedFd {
        // SAFETY: a name the call copies; the result is checked
        let fd = unsafe { sys::memfd_create(c"upcie-test".as_ptr(), sys::MFD_CLOEXEC) };
        assert!(fd >= 0);
        // SAFETY: a descriptor nobody else owns
        let fd = unsafe { OwnedFd::from_raw_fd(fd) };
        std::fs::File::from(fd.try_clone().unwrap())
            .set_len(len)
            .unwrap();
        fd
    }

    #[test]
    fn reads_what_was_written() {
        let fd = memfd(4096);
        let m = Mmio::map(fd.as_fd(), 0, 4096).unwrap();
        m.write_u32(0x10, 0xdead_beef);
        m.write_u64(0x18, 0x0123_4567_89ab_cdef);
        assert_eq!(m.read_u32(0x10), 0xdead_beef);
        assert_eq!(m.read_u64(0x18), 0x0123_4567_89ab_cdef);
        assert_eq!(m.read_u32(0x18), 0x89ab_cdef, "the low half goes first");
    }

    #[test]
    #[should_panic(expected = "outside")]
    fn refuses_past_the_end() {
        let fd = memfd(4096);
        Mmio::map(fd.as_fd(), 0, 4096).unwrap().read_u32(4096);
    }

    #[test]
    #[should_panic(expected = "unaligned")]
    fn refuses_unaligned() {
        let fd = memfd(4096);
        Mmio::map(fd.as_fd(), 0, 4096).unwrap().read_u32(2);
    }
}
