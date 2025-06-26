.. image:: https://raw.githubusercontent.com/safl/upcie/main/upcie.png
   :alt: uPCIe


uPCIe: Libraries for user-space PCIe drivers, tools for device interaction
==========================================================================

This project began as an effort to verify behavior in `vfio` and `vfio-pci`,
especially around the boundaries between safe and unsafe user-space practices.
It quickly evolved in the opposite direction — toward providing a suite of
libraries and tools primarily targeting `uio-pci-generic` and physical memory
access via Linux hugepages.

The motivation also stems from improving key aspects of the **xNVMe** project,
notably the long-neglected `xnvme-driver` script. Today, ``uPCIe`` includes
clean, explicit alternatives in the form of the ``devbind`` and ``hugepages``
tools.

Longer term, the goal is to enable minimalist NVMe driver implementations for
cases where SPDK or libvfn are unsuitable. This includes hybrid or cooperative
drivers shared between the OS kernel, user-space libraries, and accelerators.

**Topics of interest:**

- Mapping PCIe BAR spaces using `vfio-pci` / `uio-pci-generic` for device communication
- Understanding IOMMU-protected memory for safe DMA
- Using hugepages for unsafe DMA in concert with `uio-pci-generic` and `vfio-pci`
- Managing PCIe driver binding and unbinding
- Interrupts and how to handle them

The intent is to keep things general and decoupled. However, certain components
favor NVMe use cases, such as the ``devbind`` tool.

Libraries
=========

The **C** libraries are:

- **Header-only**
- **Idiomatic**
- **Zero-dependency**
- **Minimalistic**
- **Low-Coupling**
- **C99/C11** compatible

All headers are conveniently bundled in the umbrella header ``upcie.h``.
You may include this umbrella header for ease of use, or selectively include
only what you need, depending on your toolchain and UAPI requirements.

Descriptions of the individual components are provided below.

bitfield.h
----------

Provides macros and helpers for working with bitfields. Simplifies extraction,
masking, shifting, and printing of fields packed within integers or registers.

hostmem.h
---------

Top-level entry point for host memory management. Delegates to lower-level
components for hugepage allocation, heap management, and DMA buffer allocation.

hostmem_dma.h
-------------

Exposes a malloc-like interface for allocating and freeing DMA-capable
memory buffers. Designed for ease of use while remaining composable with
lower-level components.

hostmem_heap.h
--------------

Implements a simple heap allocator over a hugepage-backed memory region.
Supports multiple allocations and virtual-to-physical resolution per block.

hostmem_hugepage.h
------------------

Manages physically contiguous memory using Linux hugepages (via hugetlbfs or
memfd). Supports allocation, deallocation, and physical address resolution.
Ideal for direct hardware access or P2P DMA scenarios.

mmio.h
------

Provides volatile 32-bit and 64-bit load/store helpers for MMIO access.
Suited for interacting with PCI BARs, device registers, and low-level
memory-mapped interfaces in user space.

pci.h
-----

Supports PCI device discovery, BDF parsing/formatting, and BAR mapping.
Useful for building user-space drivers or tools that interact with PCIe devices.

upcie.h
-------

Umbrella header that includes all uPCIe library components. Intended for
convenient access to all functionality, especially during prototyping
or exploratory development.

vfioctl.h
---------

Wraps Linux VFIO IOCTLs with convenience helpers and structs for managing
containers, IOMMU groups, and devices. Streamlines interaction with VFIO
from user space.

Extension: NVMe Driver Components
=================================

These header-only components form the foundation of a minimal user-space NVMe
driver. They are idiomatic, composable, and integrate with uPCIe memory and
PCIe abstractions.

Like the other headers, they are intentionally minimal, supporting only basic
NVMe interaction over PCIe. RDMA and TCP are not included. The following
features are within scope but not yet implemented:

**TODO**

- PRP list helpers
- SGL list helpers

For available functionality and components, see the description
below. And a usage example is provided in `upcie_nvme_driver.c <https://github.com/safl/upcie/blob/main/example/upcie_nvme_driver.c>`_

nvme_mmio.h
-----------

Defines accessors for MMIO-based NVMe controller registers. Provides symbolic
offsets and structured register views for CAP, VS, CC, CSTS, AQA, and DB
registers. Built on top of mmio.h and designed for safe and explicit
controller initialization and runtime access.

nvme_controller.h
-----------------

Defines a struct nvme_controller that wraps BAR access, admin queue setup,
and controller reset logic. Encapsulates register access and bootstrapping
flow. Intended as the high-level entry point for interacting with an NVMe
device.

nvme_qpair.h
------------

Defines a struct nvme_qpair for representing submission and completion
queues. Supports allocation, initialization, and doorbell management. Includes
helpers for queue setup, teardown, and pointer movement. Works with both admin
and I/O queues.

nvme_command.h
--------------

Defines the NVMe command format and related structures. Includes helpers for
initializing common admin and I/O commands. Meant to be shared between
subsystems issuing NVMe requests.

nvme_request.h
--------------

Provides struct nvme_request for managing the lifecycle of a single NVMe
command — including metadata, payload, and completion tracking. Designed to
support synchronous or polled submission models.

nvme_qid.h
----------

Defines an abstraction for queue identifiers (QID). Tracks queue types,
indexes, and roles (admin, io, shared, etc). Allows clean referencing of
queues by their logical purpose rather than raw integers.

Tools
=====

The **devbind** tool is a Python-based CLI utility. It is designed to be usable
as a self-contained, standalone script that you can download and run with a
reasonably modern Python interpreter on your system.

devbind
-------

A utility to list PCIe devices and their current driver association, with
functionality to unbind and bind drivers. This is currently a self-contained
Python script, with the intent of evolving it into a replacement for the
``xnvme-driver`` script.

hugepages
---------

A command-line tool for inspecting hugepage support, reserving hugepages, and
mounting hugetlbfs. It is not sophisticated, just a convenient alternative to
manually working with sysfs paths.
