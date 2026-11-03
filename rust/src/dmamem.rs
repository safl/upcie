// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

//! Memory a device reaches by DMA, and the address it reaches it at
//!
//! What `<upcie/dmamem_memfd.h>` and `<upcie/hostmem_hugepage.h>` do for C, on the two paths
//! uPCIe supports:
//!
//! - with an IOMMU, [`DmaMem::in_ioas`] backs a buffer with a memfd and maps it into an I/O
//!   address space at an IOVA of the caller's choosing;
//! - without one, [`DmaMem::hugepage`] takes one 2 MiB hugepage, locked, whose physical address
//!   the device uses directly, as `uio_pci_generic` needs. Reading the physical address needs
//!   `CAP_SYS_ADMIN`.
//!
//! The device may write the buffer at any time a command lets it, so the buffer is never handed
//! out as a Rust slice, which would promise that nothing changes it. It is read and written by
//! copying, with volatile accesses ordered against the device by [`crate::barrier`].

use std::fmt;
use std::fs::File;
use std::io;
use std::os::fd::{AsFd, AsRawFd, FromRawFd, OwnedFd};
use std::os::unix::fs::FileExt;
use std::ptr::NonNull;

use crate::barrier::{dma_rmb, dma_wmb};
use crate::iommufd::{Access, Ioas, Mapping};
use crate::sys;

/// The size of the hugepage the raw-physical path takes
pub const HUGEPAGE: usize = 2 << 20;

/// A buffer the device reaches at [`DmaMem::iova`]; unmapped from the device, then freed, on drop
pub struct DmaMem {
    base: sys::Base,
    len: usize,
    iova: u64,
    mapping: Option<Mapping<'static>>,
    _memfd: Option<OwnedFd>,
}

fn map_memory(fd: Option<&OwnedFd>, len: usize, extra: i32) -> io::Result<NonNull<u8>> {
    let (flags, raw) = match fd {
        Some(fd) => (sys::MAP_SHARED, fd.as_raw_fd()),
        None => (sys::MAP_PRIVATE | sys::MAP_ANONYMOUS, -1),
    };
    let prot = sys::PROT_READ | sys::PROT_WRITE;
    // SAFETY: a fresh mapping, of a descriptor held open or anonymous
    let ptr = unsafe {
        sys::mmap(
            std::ptr::null_mut(),
            len,
            prot,
            flags | extra | sys::MAP_POPULATE,
            raw,
            0,
        )
    };
    if ptr == sys::MAP_FAILED {
        return Err(io::Error::last_os_error());
    }
    Ok(NonNull::new(ptr.cast()).expect("mmap returned null"))
}

/// Whether mapping by file failed for want of the kernel's support, so mapping by address may
/// still work; any other failure would fail that way too
fn map_file_unsupported(err: &io::Error) -> bool {
    matches!(err.raw_os_error(), Some(sys::ENOTTY | sys::EOPNOTSUPP))
}

impl DmaMem {
    /// `len` bytes of memfd-backed memory, zeroed, mapped into `ioas` at `iova`
    ///
    /// `len` and `iova` must be multiples of the page size.
    pub fn in_ioas(ioas: &Ioas, len: usize, iova: u64, access: Access) -> io::Result<Self> {
        // SAFETY: a name the call copies; the result is checked
        let fd = unsafe { sys::memfd_create(c"upcie-dmamem".as_ptr(), sys::MFD_CLOEXEC) };
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }
        // SAFETY: a descriptor nobody else owns
        let fd = unsafe { OwnedFd::from_raw_fd(fd) };
        File::from(fd.try_clone()?).set_len(len as u64)?;
        let base = map_memory(Some(&fd), len, 0)?;
        let mut mem = Self {
            base: sys::Base(base),
            len,
            iova,
            mapping: None,
            _memfd: None,
        };
        // Kernels without IOMMU_IOAS_MAP_FILE get the process's mapping instead
        let mapping = match ioas.map_file(fd.as_fd(), 0, len, iova, access) {
            Ok(mapping) => mapping,
            Err(err) if map_file_unsupported(&err) => {
                // SAFETY: `mem` keeps the memory mapped and reaches it only through its methods
                // until its drop, which drops the device mapping first
                unsafe { ioas.map_user(base.as_ptr(), len, iova, access) }?
            }
            Err(err) => return Err(err),
        };
        mem.mapping = Some(mapping);
        mem._memfd = Some(fd);
        Ok(mem)
    }

    /// One 2 MiB hugepage, locked and zeroed, at its physical address: the raw-physical path
    ///
    /// Asks for a 2 MiB page whatever the system's default hugepage size is, so the device is
    /// given memory that is physically contiguous for all of [`HUGEPAGE`].
    pub fn hugepage() -> io::Result<DmaMem> {
        let base = map_memory(None, HUGEPAGE, sys::MAP_HUGETLB | sys::MAP_HUGE_2MB)?;
        let mut mem = DmaMem {
            base: sys::Base(base),
            len: HUGEPAGE,
            iova: 0,
            mapping: None,
            _memfd: None,
        };
        // SAFETY: the mapping just made
        if unsafe { sys::mlock(base.as_ptr().cast(), HUGEPAGE) } != 0 {
            return Err(io::Error::last_os_error());
        }
        mem.iova = physical(base.as_ptr() as u64)?;
        Ok(mem)
    }

    /// The buffer's address in this process, for whoever must hand it on without copying
    ///
    /// Reads and writes through it must be volatile, and ordered with the device by
    /// [`crate::barrier`], as [`DmaMem::read`] and [`DmaMem::write`] do.
    pub fn as_ptr(&self) -> *mut u8 {
        self.base.0.as_ptr()
    }

    /// The buffer's length in bytes
    pub fn len(&self) -> usize {
        self.len
    }

    /// Whether the buffer is empty, which no constructor makes
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// The address the device uses for the buffer's first byte
    pub fn iova(&self) -> u64 {
        self.iova
    }

    fn at(&self, off: usize, len: usize) -> *mut u8 {
        assert!(
            off.checked_add(len).is_some_and(|end| end <= self.len),
            "{len} bytes at {off:#x} lie outside a {:#x}-byte buffer",
            self.len
        );
        // SAFETY: checked to lie inside the buffer
        unsafe { self.as_ptr().add(off) }
    }

    /// Copy `buf.len()` bytes at `off` out of the buffer, after what the device wrote before
    pub fn read(&self, off: usize, buf: &mut [u8]) {
        let src = self.at(off, buf.len());
        dma_rmb();
        for (i, byte) in buf.iter_mut().enumerate() {
            // SAFETY: inside the buffer, as `at` checks
            *byte = unsafe { src.add(i).read_volatile() };
        }
    }

    /// Copy `buf` into the buffer at `off`, before whatever tells the device to look
    pub fn write(&self, off: usize, buf: &[u8]) {
        let dst = self.at(off, buf.len());
        for (i, byte) in buf.iter().enumerate() {
            // SAFETY: inside the buffer, as `at` checks
            unsafe { dst.add(i).write_volatile(*byte) };
        }
        dma_wmb();
    }

    /// The little-endian 32-bit word at `off`, as a device writes them, read in one access
    ///
    /// Reads that follow are ordered after it, so a word saying an entry is complete can be
    /// trusted for the rest of the entry.
    pub fn read_u32(&self, off: usize) -> u32 {
        let ptr = self.at(off, 4);
        assert!(off % 4 == 0, "a 32-bit read at {off:#x} is unaligned");
        // SAFETY: inside the buffer and aligned, as checked
        let val = unsafe { ptr.cast::<u32>().read_volatile() };
        dma_rmb();
        u32::from_le(val)
    }

    /// The little-endian 64-bit word at `off`
    pub fn read_u64(&self, off: usize) -> u64 {
        let mut word = [0u8; 8];
        self.read(off, &mut word);
        u64::from_le_bytes(word)
    }
}

impl fmt::Debug for DmaMem {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("DmaMem")
            .field("len", &self.len)
            .field("iova", &format_args!("{:#x}", self.iova))
            .field("mapping", &self.mapping)
            .finish()
    }
}

impl Drop for DmaMem {
    fn drop(&mut self) {
        // The device loses the memory before the process does
        drop(self.mapping.take());
        // SAFETY: the mapping the constructor made, unmapped once, after the device lost it
        unsafe { sys::munmap(self.as_ptr().cast(), self.len) };
    }
}

/// The physical address of the byte at `va` in this process, from `/proc/self/pagemap`
pub fn physical(va: u64) -> io::Result<u64> {
    let page = sys::page_size() as u64;
    let mut entry = [0u8; 8];
    File::open("/proc/self/pagemap")?.read_exact_at(&mut entry, va / page * 8)?;
    let entry = u64::from_ne_bytes(entry);
    let pfn = entry & ((1 << 55) - 1);
    // Without CAP_SYS_ADMIN the kernel reports the page present with a frame number of zero
    if entry & (1 << 63) == 0 || pfn == 0 {
        return Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            "no physical address: the page is not present, or reading it needs CAP_SYS_ADMIN",
        ));
    }
    Ok(pfn * page + va % page)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Anonymous memory standing in for a device's buffer, mapped into nothing
    fn plain(len: usize) -> DmaMem {
        DmaMem {
            base: sys::Base(map_memory(None, len, 0).unwrap()),
            len,
            iova: 0,
            mapping: None,
            _memfd: None,
        }
    }

    #[test]
    fn writes_then_reads_back() {
        let mem = plain(4096);
        mem.write(0x10, &0xdead_beef_u32.to_le_bytes());
        mem.write(0x20, &0x0123_4567_89ab_cdef_u64.to_le_bytes());
        assert_eq!(mem.read_u32(0x10), 0xdead_beef);
        assert_eq!(mem.read_u64(0x20), 0x0123_4567_89ab_cdef);
        let mut out = [0u8; 4];
        mem.read(0x10, &mut out);
        assert_eq!(out, 0xdead_beef_u32.to_le_bytes());
    }

    #[test]
    #[should_panic(expected = "outside")]
    fn refuses_a_read_past_the_end() {
        plain(4096).read(4090, &mut [0u8; 8]);
    }

    #[test]
    #[should_panic(expected = "unaligned")]
    fn refuses_an_unaligned_word() {
        plain(4096).read_u32(2);
    }

    #[test]
    fn falls_back_only_where_the_kernel_lacks_map_file() {
        let os = io::Error::from_raw_os_error;
        assert!(map_file_unsupported(&os(sys::ENOTTY)));
        assert!(map_file_unsupported(&os(sys::EOPNOTSUPP)));
        for errno in [17, 22, 12] {
            assert!(!map_file_unsupported(&os(errno)), "errno {errno}");
        }
    }

    #[test]
    fn asks_for_a_2mib_hugepage() {
        assert_eq!(sys::MAP_HUGE_2MB, 0x5400_0000);
        assert_eq!(HUGEPAGE, 1 << 21);
    }

    #[test]
    fn moves_between_threads() {
        fn send<T: Send>() {}
        send::<DmaMem>();
        send::<crate::mmio::Mmio<'static>>();
    }
}
