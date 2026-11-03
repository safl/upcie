// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

//! A small NVMe driver: identify the controller and namespace 1, read its first block
//!
//! The Rust counterpart of `example/upcie_nvme_driver.c`. The path follows the driver the
//! function is bound to: `vfio-pci` goes through its VFIO character device, iommufd and a memfd
//! buffer; `uio_pci_generic` maps BAR 0 through sysfs and gives the controller a hugepage by its
//! physical address, which needs root.
//!
//! The controller is brought up as the NVMe Base Specification lays out: disabled, admin queues
//! set, enabled, identified, the number of I/O queues set before any is created, one I/O queue
//! pair created, and everything torn down again, ending disabled. On an error the controller is
//! disabled all the same before its memory is freed. One that will not disable is cut off from
//! the memory first: by unmapping it from the IOMMU, or on the raw path by clearing its bus
//! mastering, and should even that fail, the memory stays allocated for as long as the process
//! runs, which is all user space can do.
//!
//! usage: nvme BDF

use std::error::Error;
use std::mem::ManuallyDrop;
use std::time::{Duration, Instant};

use upcie::dmamem::DmaMem;
use upcie::iommufd::{Access, Iommufd};
use upcie::mmio::Mmio;
use upcie::pci::{Bdf, Function};
use upcie::vfio::Device;

type Res<T> = Result<T, Box<dyn Error>>;

const CAP: usize = 0x00;
const CC: usize = 0x14;
const CSTS: usize = 0x1c;
const AQA: usize = 0x24;
const ASQ: usize = 0x28;
const ACQ: usize = 0x30;
const DOORBELLS: usize = 0x1000;

const ASQ_OFF: usize = 0x0000;
const ACQ_OFF: usize = 0x1000;
const IOSQ_OFF: usize = 0x2000;
const IOCQ_OFF: usize = 0x3000;
const DATA_OFF: usize = 0x4000;
const MEM_LEN: usize = 2 << 20;
const IOVA: u64 = 0x1000_0000;
const DEPTH: u16 = 32;

/// The opcodes used here, admin and NVM
mod opc {
    pub const DELETE_IO_SQ: u8 = 0x00;
    pub const CREATE_IO_SQ: u8 = 0x01;
    pub const READ: u8 = 0x02;
    pub const DELETE_IO_CQ: u8 = 0x04;
    pub const CREATE_IO_CQ: u8 = 0x05;
    pub const IDENTIFY: u8 = 0x06;
    pub const SET_FEATURES: u8 = 0x09;
}

/// A submission queue entry, with the fields this driver sets by name
#[derive(Clone, Copy, Debug, Default)]
struct Command {
    opcode: u8,
    nsid: u32,
    prp1: u64,
    cdw10: u32,
    cdw11: u32,
    cdw12: u32,
}

impl Command {
    fn new(opcode: u8) -> Self {
        Self {
            opcode,
            ..Default::default()
        }
    }

    /// The 64 bytes the controller reads, with command identifier `cid`
    fn encode(&self, cid: u16) -> [u8; 64] {
        let mut dw = [0u32; 16];
        dw[0] = u32::from(self.opcode) | u32::from(cid) << 16;
        dw[1] = self.nsid;
        dw[6] = self.prp1 as u32;
        dw[7] = (self.prp1 >> 32) as u32;
        dw[10] = self.cdw10;
        dw[11] = self.cdw11;
        dw[12] = self.cdw12;
        let mut bytes = [0u8; 64];
        for (chunk, word) in bytes.chunks_exact_mut(4).zip(dw) {
            chunk.copy_from_slice(&word.to_le_bytes());
        }
        bytes
    }
}

/// A submission and completion queue pair in the buffer
struct Queue {
    qid: u16,
    sq: usize,
    cq: usize,
    tail: u16,
    head: u16,
    phase: bool,
    cid: u16,
}

impl Queue {
    fn new(qid: u16, sq: usize, cq: usize) -> Self {
        Self {
            qid,
            sq,
            cq,
            tail: 0,
            head: 0,
            phase: true,
            cid: 0,
        }
    }
}

/// What keeps a controller that will not disable from reaching memory once it is freed
enum Cutoff {
    /// The memory is unmapped from the controller's IOMMU before it is freed
    Iommu,
    /// Nothing does but the controller's bus mastering, which is cleared through sysfs
    BusMaster(Function),
}

struct Nvme<'d> {
    regs: Mmio<'d>,
    // Freed by the drop only once the controller cannot reach it
    mem: ManuallyDrop<DmaMem>,
    cutoff: Cutoff,
    stride: usize,
    timeout: Duration,
}

impl<'d> Nvme<'d> {
    fn new(regs: Mmio<'d>, mem: DmaMem, cutoff: Cutoff) -> Res<Nvme<'d>> {
        let cap = regs.read_u64(CAP);
        let mpsmin = (cap >> 48) & 0xf;
        if mpsmin != 0 {
            return Err(format!("the controller's smallest page is {} KiB", 4 << mpsmin).into());
        }
        Ok(Nvme {
            stride: 4 << ((cap >> 32) & 0xf),
            timeout: Duration::from_millis(500 * ((cap >> 24) & 0xff).max(1)),
            regs,
            mem: ManuallyDrop::new(mem),
            cutoff,
        })
    }

    fn wait_ready(&self, ready: bool) -> Res<()> {
        let start = Instant::now();
        loop {
            let csts = self.regs.read_u32(CSTS);
            if csts & 1 == u32::from(ready) {
                return Ok(());
            }
            if csts & 2 != 0 {
                return Err("the controller reports a fatal status (CSTS.CFS)".into());
            }
            if start.elapsed() > self.timeout {
                return Err(format!("CSTS.RDY did not become {}", u8::from(ready)).into());
            }
            std::thread::sleep(Duration::from_millis(1));
        }
    }

    fn iova(&self, off: usize) -> u64 {
        self.mem.iova() + off as u64
    }

    fn doorbell(&self, qid: u16, completion: bool, value: u16) {
        let index = 2 * qid as usize + usize::from(completion);
        self.regs
            .write_u32(DOORBELLS + index * self.stride, value.into());
    }

    /// Submit `cmd` on `q` and wait for its completion; returns completion dword 0
    fn submit(&self, q: &mut Queue, cmd: Command) -> Res<u32> {
        self.mem
            .write(q.sq + q.tail as usize * 64, &cmd.encode(q.cid));
        q.tail = (q.tail + 1) % DEPTH;
        self.doorbell(q.qid, false, q.tail);

        let entry = q.cq + q.head as usize * 16;
        let start = Instant::now();
        let dw3 = loop {
            let dw3 = self.mem.read_u32(entry + 12);
            if (dw3 >> 16) & 1 == u32::from(q.phase) {
                break dw3;
            }
            if self.regs.read_u32(CSTS) & 2 != 0 {
                return Err("the controller reports a fatal status (CSTS.CFS)".into());
            }
            if start.elapsed() > self.timeout {
                return Err(format!(
                    "no completion for opcode {:#04x} on queue {}",
                    cmd.opcode, q.qid
                )
                .into());
            }
            std::hint::spin_loop();
        };
        let dw0 = self.mem.read_u32(entry);
        q.head = (q.head + 1) % DEPTH;
        if q.head == 0 {
            q.phase = !q.phase;
        }
        self.doorbell(q.qid, true, q.head);
        q.cid = q.cid.wrapping_add(1);
        match (dw3 >> 17) & 0x7fff {
            0 => Ok(dw0),
            status => {
                Err(format!("opcode {:#04x} failed with status {status:#x}", cmd.opcode).into())
            }
        }
    }

    /// `len` bytes of the data page, copied out
    fn data(&self, off: usize, len: usize) -> Vec<u8> {
        let mut out = vec![0u8; len];
        self.mem.read(DATA_OFF + off, &mut out);
        out
    }

    fn data_u32(&self, off: usize) -> u32 {
        self.mem.read_u32(DATA_OFF + off)
    }
}

impl Drop for Nvme<'_> {
    fn drop(&mut self) {
        if self.regs.read_u32(CC) & 1 != 0 {
            self.regs.write_u32(CC, 0);
        }
        let stopped = match (self.wait_ready(false), &self.cutoff) {
            (Ok(()), _) => Ok(()),
            (Err(err), Cutoff::Iommu) => {
                eprintln!("{err}; its memory is unmapped from the IOMMU before it is freed");
                Ok(())
            }
            (Err(err), Cutoff::BusMaster(func)) => {
                eprintln!("{err}; clearing its bus mastering");
                set_bus_master(
                    |b| func.config_read(4, b),
                    |b| func.config_write(4, b),
                    false,
                )
            }
        };
        match stopped {
            // SAFETY: dropped once, here, and the controller no longer reaches it
            Ok(()) => unsafe { ManuallyDrop::drop(&mut self.mem) },
            Err(err) => eprintln!("{err}; its memory stays allocated until the process exits"),
        }
    }
}

fn text(bytes: &[u8]) -> String {
    String::from_utf8_lossy(bytes).trim().to_string()
}

fn run(nvme: &Nvme<'_>) -> Res<()> {
    nvme.regs.write_u32(CC, 0);
    nvme.wait_ready(false)?;
    let depth = u32::from(DEPTH) - 1;
    nvme.regs.write_u32(AQA, depth << 16 | depth);
    nvme.regs.write_u64(ASQ, nvme.iova(ASQ_OFF));
    nvme.regs.write_u64(ACQ, nvme.iova(ACQ_OFF));
    // IOCQES 2^4, IOSQES 2^6, 4 KiB pages, the NVM command set, enabled
    nvme.regs.write_u32(CC, 4 << 20 | 6 << 16 | 1);
    nvme.wait_ready(true)?;

    let mut admin = Queue::new(0, ASQ_OFF, ACQ_OFF);
    let data = nvme.iova(DATA_OFF);

    let identify_ctrl = Command {
        prp1: data,
        cdw10: 1,
        ..Command::new(opc::IDENTIFY)
    };
    nvme.submit(&mut admin, identify_ctrl)?;
    println!(
        "model {}, firmware {}",
        text(&nvme.data(24, 40)),
        text(&nvme.data(64, 8))
    );

    let queues = Command {
        cdw10: 0x07,
        ..Command::new(opc::SET_FEATURES)
    };
    let granted = nvme.submit(&mut admin, queues)?;
    println!(
        "I/O queues granted: {} submission, {} completion",
        (granted & 0xffff) + 1,
        (granted >> 16) + 1
    );

    let create_cq = Command {
        prp1: nvme.iova(IOCQ_OFF),
        cdw10: depth << 16 | 1,
        cdw11: 1,
        ..Command::new(opc::CREATE_IO_CQ)
    };
    nvme.submit(&mut admin, create_cq)?;
    let create_sq = Command {
        prp1: nvme.iova(IOSQ_OFF),
        cdw10: depth << 16 | 1,
        cdw11: 1 << 16 | 1,
        ..Command::new(opc::CREATE_IO_SQ)
    };
    nvme.submit(&mut admin, create_sq)?;
    let mut io = Queue::new(1, IOSQ_OFF, IOCQ_OFF);

    let identify_ns = Command {
        nsid: 1,
        prp1: data,
        ..Command::new(opc::IDENTIFY)
    };
    nvme.submit(&mut admin, identify_ns)?;
    let nsze = u64::from_le_bytes(nvme.data(0, 8).try_into().expect("8 bytes"));
    let flbas = nvme.data(26, 1)[0] & 0xf;
    let lbaf = nvme.data_u32(128 + 4 * flbas as usize);
    let lbads = (lbaf >> 16) & 0xff;
    println!("namespace 1: {nsze} blocks of {} bytes", 1u64 << lbads);
    if lbads > 12 {
        return Err(format!(
            "blocks of {} bytes do not fit the one page read",
            1u64 << lbads
        )
        .into());
    }

    let read = Command {
        nsid: 1,
        prp1: data,
        ..Command::new(opc::READ)
    };
    nvme.submit(&mut io, read)?;
    let head: Vec<String> = nvme
        .data(0, 16)
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect();
    println!("block 0 starts {}", head.join(""));

    let delete_sq = Command {
        cdw10: 1,
        ..Command::new(opc::DELETE_IO_SQ)
    };
    nvme.submit(&mut admin, delete_sq)?;
    let delete_cq = Command {
        cdw10: 1,
        ..Command::new(opc::DELETE_IO_CQ)
    };
    nvme.submit(&mut admin, delete_cq)?;

    nvme.regs.write_u32(CC, 0);
    nvme.wait_ready(false)
}

/// Let the function decode memory and master the bus, or stop it mastering the bus
fn set_bus_master(
    read: impl Fn(&mut [u8]) -> std::io::Result<()>,
    write: impl Fn(&[u8]) -> std::io::Result<()>,
    on: bool,
) -> Res<()> {
    let mut cmd = [0u8; 2];
    read(&mut cmd)?;
    let cmd = u16::from_le_bytes(cmd);
    let cmd = if on { cmd | 0x6 } else { cmd & !0x4 };
    write(&cmd.to_le_bytes())?;
    Ok(())
}

fn main() -> Res<()> {
    let bdf: Bdf = std::env::args().nth(1).ok_or("usage: nvme BDF")?.parse()?;
    let func = Function::open(bdf)?;
    match func.driver()?.as_deref() {
        Some("vfio-pci") => {
            let iommufd = Iommufd::open()?;
            let ioas = iommufd.alloc_ioas()?;
            let mut dev = Device::open(func.vfio_cdev()?)?;
            dev.bind(&iommufd)?;
            dev.attach(&ioas)?;
            set_bus_master(|b| dev.config_read(4, b), |b| dev.config_write(4, b), true)?;
            let mem = DmaMem::in_ioas(&ioas, MEM_LEN, IOVA, Access::ReadWrite)?;
            println!("{bdf}: vfio-pci, iommufd");
            let nvme = Nvme::new(dev.map_region(0)?, mem, Cutoff::Iommu)?;
            run(&nvme)
        }
        Some("uio_pci_generic") => {
            set_bus_master(
                |b| func.config_read(4, b),
                |b| func.config_write(4, b),
                true,
            )?;
            println!("{bdf}: uio_pci_generic, physical addresses");
            let (regs, mem) = (func.map_bar(0)?, DmaMem::hugepage()?);
            let nvme = Nvme::new(regs, mem, Cutoff::BusMaster(func.clone()))?;
            run(&nvme)
        }
        other => Err(format!(
            "{bdf} is bound to {other:?}; bind it to vfio-pci or uio_pci_generic"
        )
        .into()),
    }
}
