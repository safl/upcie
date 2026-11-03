// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

//! PCI functions as sysfs shows them
//!
//! What `<upcie/pci.h>` does for C: addresses, identity, the driver a function is bound to, its
//! config space and BARs through sysfs. A BAR mapped this way is the raw-physical path, used with
//! `uio_pci_generic` when there is no IOMMU; with one, map regions through [`crate::vfio`].

use std::fmt;
use std::fs::{self, File, OpenOptions};
use std::io;
use std::os::fd::AsFd;
use std::os::unix::fs::FileExt;
use std::path::PathBuf;
use std::str::FromStr;

use crate::mmio::Mmio;

/// A PCI function's address: domain, bus, device and function
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Bdf {
    pub domain: u16,
    pub bus: u8,
    pub device: u8,
    pub function: u8,
}

impl FromStr for Bdf {
    type Err = io::Error;

    /// `DDDD:BB:DD.F`, or `BB:DD.F` in domain 0
    fn from_str(s: &str) -> io::Result<Self> {
        let bad = || {
            io::Error::new(
                io::ErrorKind::InvalidInput,
                format!("not a PCI address: {s:?}"),
            )
        };
        let (rest, function) = s.rsplit_once('.').ok_or_else(bad)?;
        let parts: Vec<&str> = rest.split(':').collect();
        let (domain, bus, device) = match parts[..] {
            [d, b, dev] => (d, b, dev),
            [b, dev] => ("0", b, dev),
            _ => return Err(bad()),
        };
        let hex = |t: &str, max: u32| {
            u32::from_str_radix(t, 16)
                .ok()
                .filter(|v| *v <= max)
                .ok_or_else(bad)
        };
        Ok(Self {
            domain: hex(domain, 0xffff)? as u16,
            bus: hex(bus, 0xff)? as u8,
            device: hex(device, 0x1f)? as u8,
            function: hex(function, 0x7)? as u8,
        })
    }
}

impl fmt::Display for Bdf {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "{:04x}:{:02x}:{:02x}.{:x}",
            self.domain, self.bus, self.device, self.function
        )
    }
}

/// A PCI function in sysfs
#[derive(Clone, Debug)]
pub struct Function {
    bdf: Bdf,
    path: PathBuf,
}

impl Function {
    /// The function at `bdf`, if sysfs has it
    pub fn open(bdf: Bdf) -> io::Result<Self> {
        let path = PathBuf::from(format!("/sys/bus/pci/devices/{bdf}"));
        fs::metadata(&path)?;
        Ok(Self { bdf, path })
    }

    pub fn bdf(&self) -> Bdf {
        self.bdf
    }

    fn hex_attr(&self, name: &str) -> io::Result<u32> {
        let text = fs::read_to_string(self.path.join(name))?;
        u32::from_str_radix(text.trim().trim_start_matches("0x"), 16)
            .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))
    }

    /// Vendor and device identifiers
    pub fn ids(&self) -> io::Result<(u16, u16)> {
        Ok((
            self.hex_attr("vendor")? as u16,
            self.hex_attr("device")? as u16,
        ))
    }

    /// The name of the driver the function is bound to, `None` when unbound
    pub fn driver(&self) -> io::Result<Option<String>> {
        match fs::read_link(self.path.join("driver")) {
            Ok(link) => Ok(link.file_name().map(|n| n.to_string_lossy().into_owned())),
            Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(None),
            Err(e) => Err(e),
        }
    }

    /// The VFIO character device of a function bound to vfio-pci, such as `/dev/vfio/devices/vfio0`
    pub fn vfio_cdev(&self) -> io::Result<PathBuf> {
        let dir = fs::read_dir(self.path.join("vfio-dev"))?;
        for entry in dir {
            let name = entry?.file_name();
            if name.to_string_lossy().starts_with("vfio") {
                return Ok(PathBuf::from("/dev/vfio/devices").join(name));
            }
        }
        Err(io::Error::new(
            io::ErrorKind::NotFound,
            format!("{} has no VFIO character device", self.bdf),
        ))
    }

    fn config(&self, write: bool) -> io::Result<File> {
        OpenOptions::new()
            .read(true)
            .write(write)
            .open(self.path.join("config"))
    }

    /// Read `buf.len()` bytes of config space at `off`
    pub fn config_read(&self, off: u64, buf: &mut [u8]) -> io::Result<()> {
        self.config(false)?.read_exact_at(buf, off)
    }

    /// Write `buf` to config space at `off`
    pub fn config_write(&self, off: u64, buf: &[u8]) -> io::Result<()> {
        self.config(true)?.write_all_at(buf, off)
    }

    /// The size of BAR `bar`, from the start and end sysfs lists in `resource`; 0 when absent
    pub fn bar_size(&self, bar: u8) -> io::Result<u64> {
        let text = fs::read_to_string(self.path.join("resource"))?;
        let line = text.lines().nth(bar as usize).ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::InvalidInput,
                format!("{} has no BAR {bar}", self.bdf),
            )
        })?;
        let mut fields = line
            .split_whitespace()
            .map(|t| u64::from_str_radix(t.trim_start_matches("0x"), 16));
        let (start, end) = match (fields.next(), fields.next()) {
            (Some(Ok(s)), Some(Ok(e))) => (s, e),
            _ => {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "unreadable resource line",
                ));
            }
        };
        Ok(if end > start { end - start + 1 } else { 0 })
    }

    /// Map BAR `bar` through sysfs, the raw-physical path
    pub fn map_bar(&self, bar: u8) -> io::Result<Mmio<'static>> {
        let size = self.bar_size(bar)?;
        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .open(self.path.join(format!("resource{bar}")))?;
        Mmio::map(file.as_fd(), 0, size as usize)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_and_prints_addresses() {
        let b: Bdf = "0000:81:00.3".parse().unwrap();
        assert_eq!((b.domain, b.bus, b.device, b.function), (0, 0x81, 0, 3));
        assert_eq!(b.to_string(), "0000:81:00.3");
        assert_eq!(
            "01:1f.7".parse::<Bdf>().unwrap().to_string(),
            "0000:01:1f.7"
        );
    }

    #[test]
    fn refuses_what_is_not_an_address() {
        for bad in [
            "",
            "81:00",
            "0000:81:20.0",
            "0000:81:00.8",
            "x:81:00.0",
            "0:0:81:00.0",
        ] {
            assert!(bad.parse::<Bdf>().is_err(), "{bad:?} parsed");
        }
    }
}
