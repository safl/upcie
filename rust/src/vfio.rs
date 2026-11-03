// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

//! A device through its VFIO character device, bound to iommufd
//!
//! What `<upcie/vfio_cdev.h>` and `<upcie/vfioctl.h>` do for C: open `/dev/vfio/devices/vfioN`,
//! bind it to an [`Iommufd`] and attach it to an [`Ioas`], then reach its regions, its config
//! space and its interrupts. An attached device holds a clone of its address space, so the
//! address space outlives the attachment, and closing the device detaches it.

use std::fs::{File, OpenOptions};
use std::io;
use std::os::fd::{AsFd, AsRawFd, BorrowedFd, FromRawFd, OwnedFd};
use std::os::unix::fs::FileExt;
use std::path::Path;

use crate::iommufd::{Ioas, Iommufd};
use crate::mmio::Mmio;
use crate::sys;

/// A region of the device: one of its BARs, its ROM or its config space
#[derive(Clone, Copy, Debug)]
pub struct Region {
    pub index: u32,
    pub flags: u32,
    pub size: u64,
    pub offset: u64,
}

impl Region {
    /// Whether the region may be mapped, which VFIO refuses for config space and I/O BARs
    pub fn mappable(&self) -> bool {
        self.flags & sys::VFIO_REGION_INFO_FLAG_MMAP != 0
    }
}

/// Interrupt types, as VFIO indexes them for a PCI device
#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Irq {
    Intx = 0,
    Msi = 1,
    Msix = 2,
}

/// What VFIO says of a device
#[derive(Clone, Copy, Debug)]
pub struct DeviceInfo {
    pub flags: u32,
    pub regions: u32,
    pub irqs: u32,
}

impl DeviceInfo {
    /// Whether VFIO has a way to reset the device, which [`Device::reset`] needs
    pub fn can_reset(&self) -> bool {
        self.flags & sys::VFIO_DEVICE_FLAGS_RESET != 0
    }
}

/// A device opened through its VFIO character device
#[derive(Debug)]
pub struct Device {
    // Before `ioas`, so the device is closed, and so detached, before its address space goes
    file: File,
    ioas: Option<Ioas>,
}

impl Device {
    /// Open the character device at `path`, as [`crate::pci::Function::vfio_cdev`] gives it
    pub fn open(path: impl AsRef<Path>) -> io::Result<Self> {
        Ok(Self {
            file: OpenOptions::new().read(true).write(true).open(path)?,
            ioas: None,
        })
    }

    /// Bind the device to `iommufd`; returns the device's identifier there
    pub fn bind(&self, iommufd: &Iommufd) -> io::Result<u32> {
        let mut arg = sys::vfio_device_bind_iommufd {
            argsz: size_of::<sys::vfio_device_bind_iommufd>() as u32,
            iommufd: iommufd.as_fd().as_raw_fd(),
            ..Default::default()
        };
        // SAFETY: the structure VFIO_DEVICE_BIND_IOMMUFD takes
        unsafe {
            sys::ioctl_ptr(
                self.file.as_raw_fd(),
                sys::VFIO_DEVICE_BIND_IOMMUFD,
                &mut arg,
            )
        }?;
        Ok(arg.out_devid)
    }

    /// Attach the bound device to `ioas`, after which its DMA goes through the address space
    ///
    /// The device keeps the address space until it is detached or closed.
    pub fn attach(&mut self, ioas: &Ioas) -> io::Result<()> {
        let mut arg = sys::vfio_device_attach_iommufd_pt {
            argsz: size_of::<sys::vfio_device_attach_iommufd_pt>() as u32,
            pt_id: ioas.id(),
            ..Default::default()
        };
        // SAFETY: the structure VFIO_DEVICE_ATTACH_IOMMUFD_PT takes
        unsafe {
            sys::ioctl_ptr(
                self.file.as_raw_fd(),
                sys::VFIO_DEVICE_ATTACH_IOMMUFD_PT,
                &mut arg,
            )
        }?;
        self.ioas = Some(ioas.clone());
        Ok(())
    }

    /// Detach the device from its address space, after which it can reach no memory
    pub fn detach(&mut self) -> io::Result<()> {
        let mut arg = sys::vfio_device_detach_iommufd_pt {
            argsz: size_of::<sys::vfio_device_detach_iommufd_pt>() as u32,
            ..Default::default()
        };
        // SAFETY: the structure VFIO_DEVICE_DETACH_IOMMUFD_PT takes
        unsafe {
            sys::ioctl_ptr(
                self.file.as_raw_fd(),
                sys::VFIO_DEVICE_DETACH_IOMMUFD_PT,
                &mut arg,
            )
        }?;
        self.ioas = None;
        Ok(())
    }

    /// The address space the device is attached to
    pub fn ioas(&self) -> Option<&Ioas> {
        self.ioas.as_ref()
    }

    /// The device's flags, and the number of regions and interrupt types it has
    pub fn info(&self) -> io::Result<DeviceInfo> {
        let mut arg = sys::vfio_device_info {
            argsz: size_of::<sys::vfio_device_info>() as u32,
            ..Default::default()
        };
        // SAFETY: the structure VFIO_DEVICE_GET_INFO takes
        unsafe { sys::ioctl_ptr(self.file.as_raw_fd(), sys::VFIO_DEVICE_GET_INFO, &mut arg) }?;
        Ok(DeviceInfo {
            flags: arg.flags,
            regions: arg.num_regions,
            irqs: arg.num_irqs,
        })
    }

    /// Region `index`: BAR 0 to 5, 6 the ROM, 7 config space
    pub fn region(&self, index: u32) -> io::Result<Region> {
        let mut arg = sys::vfio_region_info {
            argsz: size_of::<sys::vfio_region_info>() as u32,
            index,
            ..Default::default()
        };
        // SAFETY: the structure VFIO_DEVICE_GET_REGION_INFO takes
        unsafe {
            sys::ioctl_ptr(
                self.file.as_raw_fd(),
                sys::VFIO_DEVICE_GET_REGION_INFO,
                &mut arg,
            )
        }?;
        Ok(Region {
            index,
            flags: arg.flags,
            size: arg.size,
            offset: arg.offset,
        })
    }

    /// Map region `index`, which must be mappable; the mapping borrows the device, as closing
    /// the device unmaps it
    ///
    /// ```
    /// fn bar0(dev: &upcie::vfio::Device) -> std::io::Result<upcie::mmio::Mmio<'_>> {
    ///     dev.map_region(0)
    /// }
    /// ```
    ///
    /// so a mapping cannot outlive its device:
    ///
    /// ```compile_fail
    /// fn bar0(dev: upcie::vfio::Device) -> upcie::mmio::Mmio<'static> {
    ///     dev.map_region(0).unwrap()
    /// }
    /// ```
    pub fn map_region(&self, index: u32) -> io::Result<Mmio<'_>> {
        let region = self.region(index)?;
        if !region.mappable() {
            return Err(io::Error::new(
                io::ErrorKind::Unsupported,
                format!("region {index} cannot be mapped"),
            ));
        }
        Mmio::map(self.file.as_fd(), region.offset, region.size as usize)
    }

    /// Read `buf.len()` bytes of the region `index` at `off`, through the device file
    pub fn read_region(&self, index: u32, off: u64, buf: &mut [u8]) -> io::Result<()> {
        let region = self.region(index)?;
        self.file.read_exact_at(buf, region.offset + off)
    }

    /// Write `buf` to the region `index` at `off`, through the device file
    pub fn write_region(&self, index: u32, off: u64, buf: &[u8]) -> io::Result<()> {
        let region = self.region(index)?;
        self.file.write_all_at(buf, region.offset + off)
    }

    /// Read config space, which VFIO filters and partly virtualizes
    pub fn config_read(&self, off: u64, buf: &mut [u8]) -> io::Result<()> {
        self.read_region(sys::VFIO_PCI_CONFIG_REGION_INDEX, off, buf)
    }

    /// Write config space, which VFIO filters and partly virtualizes
    pub fn config_write(&self, off: u64, buf: &[u8]) -> io::Result<()> {
        self.write_region(sys::VFIO_PCI_CONFIG_REGION_INDEX, off, buf)
    }

    /// How many vectors of `irq` the device has
    pub fn irq_count(&self, irq: Irq) -> io::Result<u32> {
        let mut arg = sys::vfio_irq_info {
            argsz: size_of::<sys::vfio_irq_info>() as u32,
            index: irq as u32,
            ..Default::default()
        };
        // SAFETY: the structure VFIO_DEVICE_GET_IRQ_INFO takes
        unsafe {
            sys::ioctl_ptr(
                self.file.as_raw_fd(),
                sys::VFIO_DEVICE_GET_IRQ_INFO,
                &mut arg,
            )
        }?;
        Ok(arg.count)
    }

    /// Enable `count` vectors of `irq`, each signalling an eventfd of its own, which are returned
    ///
    /// The eventfds block on read until their vector fires; set `O_NONBLOCK` on them to poll.
    pub fn enable_irqs(&self, irq: Irq, count: u32) -> io::Result<Vec<OwnedFd>> {
        if count == 0 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "no vectors to enable; disable_irqs turns them off",
            ));
        }
        let mut fds = Vec::with_capacity(count as usize);
        for _ in 0..count {
            // SAFETY: eventfd has no preconditions; the result is checked
            let fd = unsafe { sys::eventfd(0, sys::EFD_CLOEXEC) };
            if fd < 0 {
                return Err(io::Error::last_os_error());
            }
            // SAFETY: a descriptor nobody else owns
            fds.push(unsafe { OwnedFd::from_raw_fd(fd) });
        }
        let mut words = irq_set_words(irq, &fds);
        // SAFETY: a vfio_irq_set followed by its `count` descriptors, as VFIO_DEVICE_SET_IRQS takes
        unsafe {
            sys::ioctl_ptr(
                self.file.as_raw_fd(),
                sys::VFIO_DEVICE_SET_IRQS,
                words.as_mut_ptr(),
            )
        }?;
        Ok(fds)
    }

    /// Disable every vector of `irq`
    pub fn disable_irqs(&self, irq: Irq) -> io::Result<()> {
        let mut set = sys::vfio_irq_set {
            argsz: size_of::<sys::vfio_irq_set>() as u32,
            flags: sys::VFIO_IRQ_SET_DATA_NONE | sys::VFIO_IRQ_SET_ACTION_TRIGGER,
            index: irq as u32,
            ..Default::default()
        };
        // SAFETY: the structure VFIO_DEVICE_SET_IRQS takes with no data
        unsafe { sys::ioctl_ptr(self.file.as_raw_fd(), sys::VFIO_DEVICE_SET_IRQS, &mut set) }?;
        Ok(())
    }

    /// Reset the device, when VFIO has a way to, as [`DeviceInfo::can_reset`] says
    pub fn reset(&self) -> io::Result<()> {
        // SAFETY: VFIO_DEVICE_RESET takes no argument
        unsafe { sys::ioctl_none(self.file.as_raw_fd(), sys::VFIO_DEVICE_RESET) }?;
        Ok(())
    }
}

/// The argument of VFIO_DEVICE_SET_IRQS triggering `fds`: a `vfio_irq_set`, then one descriptor
/// per vector, each a 32-bit word
fn irq_set_words(irq: Irq, fds: &[OwnedFd]) -> Vec<u32> {
    let head = size_of::<sys::vfio_irq_set>() / 4;
    let mut words = Vec::with_capacity(head + fds.len());
    words.extend([
        ((head + fds.len()) * 4) as u32,
        sys::VFIO_IRQ_SET_DATA_EVENTFD | sys::VFIO_IRQ_SET_ACTION_TRIGGER,
        irq as u32,
        0,
        fds.len() as u32,
    ]);
    words.extend(fds.iter().map(|fd| fd.as_raw_fd() as u32));
    words
}

impl AsFd for Device {
    fn as_fd(&self) -> BorrowedFd<'_> {
        self.file.as_fd()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn refuses_to_enable_no_vectors() {
        let dev = Device::open("/dev/null").unwrap();
        let err = dev.enable_irqs(Irq::Msix, 0).unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::InvalidInput);
    }

    #[test]
    fn tells_whether_it_can_reset() {
        let info = |flags| DeviceInfo {
            flags,
            regions: 9,
            irqs: 5,
        };
        assert!(info(sys::VFIO_DEVICE_FLAGS_RESET | 1 << 1).can_reset());
        assert!(!info(1 << 1).can_reset());
    }

    #[test]
    fn irq_set_layout() {
        // SAFETY: eventfd has no preconditions; the results are checked
        let fds: Vec<OwnedFd> = (0..3)
            .map(|_| {
                let fd = unsafe { sys::eventfd(0, sys::EFD_CLOEXEC) };
                assert!(fd >= 0);
                // SAFETY: a descriptor nobody else owns
                unsafe { OwnedFd::from_raw_fd(fd) }
            })
            .collect();
        let words = irq_set_words(Irq::Msix, &fds);
        assert_eq!(words.len(), 5 + 3);
        assert_eq!(
            words[0] as usize,
            size_of::<sys::vfio_irq_set>() + 3 * 4,
            "argsz covers the descriptors"
        );
        assert_eq!(words[2], 2, "MSI-X is index 2");
        assert_eq!(words[4], 3);
        assert_eq!(
            words[5..],
            fds.iter()
                .map(|fd| fd.as_raw_fd() as u32)
                .collect::<Vec<_>>()[..]
        );
    }
}
