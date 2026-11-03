// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

//! iommufd: I/O address spaces and what is mapped into them
//!
//! What `<upcie/iommufd.h>` does for C. An [`Ioas`] is an I/O address space a device is attached
//! to through [`crate::vfio`]; memory mapped into it at an IOVA is what the device reaches there.
//! Every mapping is a [`Mapping`], which unmaps its range when dropped, so the device loses the
//! memory before anything else can happen to it.
//!
//! [`Iommufd`] and [`Ioas`] are handles, cheap to clone. Whatever needs an address space to stay
//! holds a clone of it: each mapping does, and so does a device attached to it, so the address
//! space is destroyed only once the last of them is gone, and values using it can be kept
//! together in one structure.

use std::fmt;
use std::fs::{File, OpenOptions};
use std::io;
use std::marker::PhantomData;
use std::os::fd::{AsFd, AsRawFd, BorrowedFd};
use std::sync::Arc;

use crate::sys;

/// An open `/dev/iommu`, closed once the last clone of it and of its address spaces is dropped
#[derive(Clone, Debug)]
pub struct Iommufd {
    file: Arc<File>,
}

impl Iommufd {
    /// Open `/dev/iommu`
    pub fn open() -> io::Result<Self> {
        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .open("/dev/iommu")?;
        Ok(Self {
            file: Arc::new(file),
        })
    }

    /// Allocate an I/O address space, destroyed once the last clone of the [`Ioas`] is dropped
    pub fn alloc_ioas(&self) -> io::Result<Ioas> {
        let mut arg = sys::iommu_ioas_alloc {
            size: size_of::<sys::iommu_ioas_alloc>() as u32,
            ..Default::default()
        };
        // SAFETY: the structure IOMMU_IOAS_ALLOC takes, on an open iommufd
        unsafe { sys::ioctl_ptr(self.file.as_raw_fd(), sys::IOMMU_IOAS_ALLOC, &mut arg) }?;
        Ok(Ioas {
            inner: Arc::new(IoasInner {
                iommufd: self.clone(),
                id: arg.out_ioas_id,
            }),
        })
    }
}

impl AsFd for Iommufd {
    fn as_fd(&self) -> BorrowedFd<'_> {
        self.file.as_fd()
    }
}

struct IoasInner {
    iommufd: Iommufd,
    id: u32,
}

impl Drop for IoasInner {
    fn drop(&mut self) {
        let mut arg = sys::iommu_destroy {
            size: size_of::<sys::iommu_destroy>() as u32,
            id: self.id,
        };
        let fd = self.iommufd.file.as_raw_fd();
        // SAFETY: the structure IOMMU_DESTROY takes. No mapping or attached device is left, as
        // each would hold a clone; a failure leaves the object to the iommufd's close.
        let _ = unsafe { sys::ioctl_ptr(fd, sys::IOMMU_DESTROY, &mut arg) };
    }
}

/// An I/O address space
#[derive(Clone)]
pub struct Ioas {
    inner: Arc<IoasInner>,
}

impl fmt::Debug for Ioas {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Ioas").field("id", &self.inner.id).finish()
    }
}

/// Which way a device may move data through a mapping
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Access {
    /// The device reads only
    Read,
    /// The device reads and writes
    ReadWrite,
}

impl Access {
    fn flags(self) -> u32 {
        let write = match self {
            Access::Read => 0,
            Access::ReadWrite => sys::IOMMU_IOAS_MAP_WRITEABLE,
        };
        sys::IOMMU_IOAS_MAP_FIXED_IOVA | sys::IOMMU_IOAS_MAP_READABLE | write
    }
}

/// A range of an I/O address space with memory behind it, unmapped on drop
///
/// `'m` is the borrow of the memory for [`Ioas::map_slice`], and `'static` for mappings that
/// borrow nothing.
#[must_use = "the range is unmapped as soon as the Mapping is dropped"]
pub struct Mapping<'m> {
    ioas: Ioas,
    iova: u64,
    len: usize,
    _memory: PhantomData<&'m mut [u8]>,
}

impl Mapping<'_> {
    /// Where the device reaches the memory
    pub fn iova(&self) -> u64 {
        self.iova
    }

    /// How many bytes are mapped
    pub fn len(&self) -> usize {
        self.len
    }

    /// Whether nothing is mapped, which no mapping call makes
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }
}

impl fmt::Debug for Mapping<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Mapping")
            .field("ioas", &self.ioas.inner.id)
            .field("iova", &format_args!("{:#x}", self.iova))
            .field("len", &self.len)
            .finish()
    }
}

impl Drop for Mapping<'_> {
    fn drop(&mut self) {
        let mut arg = sys::iommu_ioas_unmap {
            size: size_of::<sys::iommu_ioas_unmap>() as u32,
            ioas_id: self.ioas.inner.id,
            iova: self.iova,
            length: self.len as u64,
        };
        // SAFETY: the structure IOMMU_IOAS_UNMAP takes; a failure leaves the range to the
        // address space's destruction
        let _ = unsafe { sys::ioctl_ptr(self.ioas.fd(), sys::IOMMU_IOAS_UNMAP, &mut arg) };
    }
}

impl Ioas {
    /// The address space's object identifier, which a device attaches to
    pub fn id(&self) -> u32 {
        self.inner.id
    }

    fn fd(&self) -> i32 {
        self.inner.iommufd.file.as_raw_fd()
    }

    fn map_arg(&self, va: *mut u8, len: usize, iova: u64, access: Access) -> sys::iommu_ioas_map {
        sys::iommu_ioas_map {
            size: size_of::<sys::iommu_ioas_map>() as u32,
            flags: access.flags(),
            ioas_id: self.inner.id,
            user_va: va as u64,
            length: len as u64,
            iova,
            ..Default::default()
        }
    }

    fn mapping<'m>(&self, iova: u64, len: usize) -> Mapping<'m> {
        Mapping {
            ioas: self.clone(),
            iova,
            len,
            _memory: PhantomData,
        }
    }

    /// Map `buf` to `iova` for as long as the returned [`Mapping`] lives
    ///
    /// The slice stays borrowed, mutably, until the mapping is dropped, so nothing in the
    /// program touches the memory while the device may. `buf` and `iova` must be page aligned.
    pub fn map_slice<'m>(
        &self,
        buf: &'m mut [u8],
        iova: u64,
        access: Access,
    ) -> io::Result<Mapping<'m>> {
        let len = buf.len();
        let mut arg = self.map_arg(buf.as_mut_ptr(), len, iova, access);
        // SAFETY: the structure IOMMU_IOAS_MAP takes, over memory borrowed for as long as the
        // returned mapping keeps it mapped
        unsafe { sys::ioctl_ptr(self.fd(), sys::IOMMU_IOAS_MAP, &mut arg) }?;
        Ok(self.mapping(iova, len))
    }

    /// Map `len` bytes of this process's memory at `va` to `iova`, pinning it
    ///
    /// # Safety
    ///
    /// `va..va + len` must be mapped memory that stays mapped, and that the program does not
    /// otherwise use while the device may, until the returned [`Mapping`] is dropped. Prefer
    /// [`Ioas::map_slice`], which the borrow checker holds to that, or [`crate::dmamem::DmaMem`].
    pub unsafe fn map_user(
        &self,
        va: *mut u8,
        len: usize,
        iova: u64,
        access: Access,
    ) -> io::Result<Mapping<'static>> {
        let mut arg = self.map_arg(va, len, iova, access);
        // SAFETY: the structure IOMMU_IOAS_MAP takes; the caller vouches for the memory
        unsafe { sys::ioctl_ptr(self.fd(), sys::IOMMU_IOAS_MAP, &mut arg) }?;
        Ok(self.mapping(iova, len))
    }

    /// Map `len` bytes of the file `fd` from `start` to `iova`, a memfd for one
    ///
    /// The kernel pins the file's pages itself, so nothing of this process has to stay mapped.
    /// Needs a kernel with `IOMMU_IOAS_MAP_FILE`, 6.13 or later; an older one fails with
    /// `ENOTTY`.
    pub fn map_file(
        &self,
        fd: BorrowedFd<'_>,
        start: u64,
        len: usize,
        iova: u64,
        access: Access,
    ) -> io::Result<Mapping<'static>> {
        let mut arg = sys::iommu_ioas_map_file {
            size: size_of::<sys::iommu_ioas_map_file>() as u32,
            flags: access.flags(),
            ioas_id: self.inner.id,
            fd: fd.as_raw_fd(),
            start,
            length: len as u64,
            iova,
        };
        // SAFETY: the structure IOMMU_IOAS_MAP_FILE takes, with a descriptor the caller holds
        unsafe { sys::ioctl_ptr(self.fd(), sys::IOMMU_IOAS_MAP_FILE, &mut arg) }?;
        Ok(self.mapping(iova, len))
    }
}
