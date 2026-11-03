// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

//! The few C library functions and Linux UAPI definitions the crate uses.
//!
//! uPCIe depends on nothing beyond the Linux UAPI, and the crate keeps to that: the C library is
//! linked by `std` already, so its functions are declared here rather than taken from a crate.
//! Values are those of `<linux/vfio.h>`, `<linux/iommufd.h>`, `<sys/mman.h>` and
//! `<linux/memfd.h>` on Linux x86_64 and aarch64; every structure's size is checked against the
//! UAPI headers at compile time.

#![allow(non_camel_case_types)]

use std::ffi::{c_char, c_int, c_long, c_uint, c_ulong, c_void};
use std::io;
use std::ptr::NonNull;

unsafe extern "C" {
    pub fn ioctl(fd: c_int, request: c_ulong, ...) -> c_int;
    pub fn mmap(
        addr: *mut c_void,
        len: usize,
        prot: c_int,
        flags: c_int,
        fd: c_int,
        off: i64,
    ) -> *mut c_void;
    pub fn munmap(addr: *mut c_void, len: usize) -> c_int;
    pub fn memfd_create(name: *const c_char, flags: c_uint) -> c_int;
    pub fn eventfd(initval: c_uint, flags: c_int) -> c_int;
    pub fn mlock(addr: *const c_void, len: usize) -> c_int;
    pub fn sysconf(name: c_int) -> c_long;
}

pub const PROT_READ: c_int = 0x1;
pub const PROT_WRITE: c_int = 0x2;
pub const MAP_SHARED: c_int = 0x01;
pub const MAP_PRIVATE: c_int = 0x02;
pub const MAP_ANONYMOUS: c_int = 0x20;
pub const MAP_POPULATE: c_int = 0x8000;
pub const MAP_HUGETLB: c_int = 0x40000;
/// A 2 MiB hugepage, rather than the system's default size: 21, its log2, at `MAP_HUGE_SHIFT`
pub const MAP_HUGE_2MB: c_int = 21 << 26;
pub const MAP_FAILED: *mut c_void = !0usize as *mut c_void;
pub const MFD_CLOEXEC: c_uint = 0x1;
pub const EFD_CLOEXEC: c_int = 0o2000000;
pub const SC_PAGESIZE: c_int = 30;
pub const ENOTTY: i32 = 25;
pub const EOPNOTSUPP: i32 = 95;

/// The base of memory a value owns and reaches only through its own methods
///
/// Raw pointers are neither `Send` nor `Sync`. This one is `Send`, so the values holding it may
/// move between threads; it is not `Sync`, so they are not shared between threads unless they
/// say so themselves, and any other field still has its say in both.
#[derive(Clone, Copy, Debug)]
pub struct Base(pub NonNull<u8>);

// SAFETY: the memory belongs to the value holding the pointer, whichever thread holds the value
unsafe impl Send for Base {}

/// An ioctl with a pointer to its argument, as the kernel's structures take them
///
/// # Safety
///
/// `arg` must be the structure `request` expects, and `fd` an open descriptor.
pub unsafe fn ioctl_ptr<T>(fd: c_int, request: c_ulong, arg: *mut T) -> io::Result<c_int> {
    // SAFETY: the caller passes the structure the request expects
    let ret = unsafe { ioctl(fd, request, arg) };
    if ret < 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(ret)
    }
}

/// An ioctl with no argument, or an integer one
///
/// # Safety
///
/// `fd` must be an open descriptor, and `request` one that takes no pointer.
pub unsafe fn ioctl_none(fd: c_int, request: c_ulong) -> io::Result<c_int> {
    // SAFETY: the request takes no pointer
    let ret = unsafe { ioctl(fd, request) };
    if ret < 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(ret)
    }
}

/// The system's page size
pub fn page_size() -> usize {
    // SAFETY: sysconf has no preconditions
    let size = unsafe { sysconf(SC_PAGESIZE) };
    if size > 0 { size as usize } else { 4096 }
}

const IOMMUFD_TYPE: c_ulong = b';' as c_ulong;
const fn io(ty: c_ulong, nr: c_ulong) -> c_ulong {
    (ty << 8) | nr
}
pub const IOMMU_DESTROY: c_ulong = io(IOMMUFD_TYPE, 0x80);
pub const IOMMU_IOAS_ALLOC: c_ulong = io(IOMMUFD_TYPE, 0x81);
pub const IOMMU_IOAS_MAP: c_ulong = io(IOMMUFD_TYPE, 0x85);
pub const IOMMU_IOAS_UNMAP: c_ulong = io(IOMMUFD_TYPE, 0x86);
pub const IOMMU_IOAS_MAP_FILE: c_ulong = io(IOMMUFD_TYPE, 0x8f);
pub const IOMMU_IOAS_MAP_FIXED_IOVA: u32 = 1 << 0;
pub const IOMMU_IOAS_MAP_WRITEABLE: u32 = 1 << 1;
pub const IOMMU_IOAS_MAP_READABLE: u32 = 1 << 2;

#[repr(C)]
#[derive(Default)]
pub struct iommu_destroy {
    pub size: u32,
    pub id: u32,
}

#[repr(C)]
#[derive(Default)]
pub struct iommu_ioas_alloc {
    pub size: u32,
    pub flags: u32,
    pub out_ioas_id: u32,
}

#[repr(C)]
#[derive(Default)]
pub struct iommu_ioas_map {
    pub size: u32,
    pub flags: u32,
    pub ioas_id: u32,
    pub reserved: u32,
    pub user_va: u64,
    pub length: u64,
    pub iova: u64,
}

#[repr(C)]
#[derive(Default)]
pub struct iommu_ioas_map_file {
    pub size: u32,
    pub flags: u32,
    pub ioas_id: u32,
    pub fd: i32,
    pub start: u64,
    pub length: u64,
    pub iova: u64,
}

#[repr(C)]
#[derive(Default)]
pub struct iommu_ioas_unmap {
    pub size: u32,
    pub ioas_id: u32,
    pub iova: u64,
    pub length: u64,
}

const VFIO_TYPE: c_ulong = b';' as c_ulong;
const VFIO_BASE: c_ulong = 100;
pub const VFIO_DEVICE_GET_INFO: c_ulong = io(VFIO_TYPE, VFIO_BASE + 7);
pub const VFIO_DEVICE_GET_REGION_INFO: c_ulong = io(VFIO_TYPE, VFIO_BASE + 8);
pub const VFIO_DEVICE_GET_IRQ_INFO: c_ulong = io(VFIO_TYPE, VFIO_BASE + 9);
pub const VFIO_DEVICE_SET_IRQS: c_ulong = io(VFIO_TYPE, VFIO_BASE + 10);
pub const VFIO_DEVICE_RESET: c_ulong = io(VFIO_TYPE, VFIO_BASE + 11);
pub const VFIO_DEVICE_BIND_IOMMUFD: c_ulong = io(VFIO_TYPE, VFIO_BASE + 18);
pub const VFIO_DEVICE_ATTACH_IOMMUFD_PT: c_ulong = io(VFIO_TYPE, VFIO_BASE + 19);
pub const VFIO_DEVICE_DETACH_IOMMUFD_PT: c_ulong = io(VFIO_TYPE, VFIO_BASE + 20);
pub const VFIO_DEVICE_FLAGS_RESET: u32 = 1 << 0;
pub const VFIO_REGION_INFO_FLAG_MMAP: u32 = 1 << 2;
pub const VFIO_IRQ_SET_DATA_NONE: u32 = 1 << 0;
pub const VFIO_IRQ_SET_DATA_EVENTFD: u32 = 1 << 2;
pub const VFIO_IRQ_SET_ACTION_TRIGGER: u32 = 1 << 5;
pub const VFIO_PCI_CONFIG_REGION_INDEX: u32 = 7;

#[repr(C)]
#[derive(Default)]
pub struct vfio_device_bind_iommufd {
    pub argsz: u32,
    pub flags: u32,
    pub iommufd: i32,
    pub out_devid: u32,
    pub token_uuid_ptr: u64,
}

#[repr(C)]
#[derive(Default)]
pub struct vfio_device_attach_iommufd_pt {
    pub argsz: u32,
    pub flags: u32,
    pub pt_id: u32,
    pub pasid: u32,
}

#[repr(C)]
#[derive(Default)]
pub struct vfio_device_detach_iommufd_pt {
    pub argsz: u32,
    pub flags: u32,
    pub pasid: u32,
}

#[repr(C)]
#[derive(Default)]
pub struct vfio_device_info {
    pub argsz: u32,
    pub flags: u32,
    pub num_regions: u32,
    pub num_irqs: u32,
    pub cap_offset: u32,
    pub pad: u32,
}

#[repr(C)]
#[derive(Default)]
pub struct vfio_region_info {
    pub argsz: u32,
    pub flags: u32,
    pub index: u32,
    pub cap_offset: u32,
    pub size: u64,
    pub offset: u64,
}

#[repr(C)]
#[derive(Default)]
pub struct vfio_irq_info {
    pub argsz: u32,
    pub flags: u32,
    pub index: u32,
    pub count: u32,
}

/// `struct vfio_irq_set` without its trailing data, which follows it in the same buffer
#[repr(C)]
#[derive(Default)]
pub struct vfio_irq_set {
    pub argsz: u32,
    pub flags: u32,
    pub index: u32,
    pub start: u32,
    pub count: u32,
}

const _: () = {
    use std::mem::size_of;
    assert!(size_of::<iommu_destroy>() == 8);
    assert!(size_of::<iommu_ioas_alloc>() == 12);
    assert!(size_of::<iommu_ioas_map>() == 40);
    assert!(size_of::<iommu_ioas_map_file>() == 40);
    assert!(size_of::<iommu_ioas_unmap>() == 24);
    assert!(size_of::<vfio_device_bind_iommufd>() == 24);
    assert!(size_of::<vfio_device_attach_iommufd_pt>() == 16);
    assert!(size_of::<vfio_device_detach_iommufd_pt>() == 12);
    assert!(size_of::<vfio_device_info>() == 24);
    assert!(size_of::<vfio_region_info>() == 32);
    assert!(size_of::<vfio_irq_info>() == 16);
    assert!(size_of::<vfio_irq_set>() == 20);
    assert!(IOMMU_IOAS_MAP == 0x3b85 && IOMMU_IOAS_MAP_FILE == 0x3b8f);
    assert!(VFIO_DEVICE_BIND_IOMMUFD == 0x3b76 && VFIO_DEVICE_SET_IRQS == 0x3b6e);
    assert!(VFIO_DEVICE_DETACH_IOMMUFD_PT == 0x3b78);
};
