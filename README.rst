.. image:: https://raw.githubusercontent.com/safl/upcie/main/upcie.png
   :alt: uPCIe


uPCIe: Libraries for user-space PCIe drivers, tools for device interaction
==========================================================================

Began investigating `vfio` and `vfio-pci` to better understand their usage
patterns and the boundaries between safe and unsafe practices. This is partly
motivated by the need to revisit the long-neglected `xnvme-driver` script, with
the intent to rewrite it in Python and streamline its functionality.

**Topics of interest:**

- Mapping PCIe BAR spaces using `vfio-pci` / `uio-pci-generic` for device communication
- Understanding IOMMU-protected memory for safe DMA
- Using hugepages for unsafe DMA in concert with `uio-pci-generic` and `vfio-pci`
- Managing PCIe driver binding and unbinding
- Interrupts and how to handle them

The goal is to assess what is essential for integration into the **xNVMe** project.
This exploration may lead to reusable header-only libraries or be discarded
depending on its utility.

The intent is to keep things general, however, there are things which favors
NVMe such as the ``devbind`` tool.

Libraries
=========

The **C** libraries are:

- **Header-only**
- **Idiomatic**
- **Zero-dependency**
- **C99/C11** compatible

All headers are conveniently bundled in the umbrella header ``upcie.h``.
You may include this umbrella header for ease of use, or selectively include
only what you need, depending on your toolchain and UAPI requirements.

Descriptions of the individual components are provided below.

bits.h
------

Provides a simple helper for extracting right-aligned bitfields from 64-bit
integers. Useful for parsing hardware registers, protocol fields, and other
bit-packed structures.

mmio.h
------

Provides volatile 32-bit and 64-bit load/store helpers for MMIO access.
Suited for interacting with PCI BARs, device registers, and low-level
memory-mapped interfaces in user space.

hostmem.h
---------

Top-level entry point for host memory management. Delegates to lower-level
components for hugepage allocation, heap management, and DMA buffer allocation.

hostmem_hugepage.h
------------------

Manages physically contiguous memory using Linux hugepages (via hugetlbfs or
memfd). Supports allocation, deallocation, and physical address resolution.
Ideal for direct hardware access or P2P DMA scenarios.

hostmem_heap.h
--------------

Implements a simple heap allocator over a hugepage-backed memory region.
Supports multiple allocations and virtual-to-physical resolution per block.

hostmem_dma.h
-------------

Exposes a malloc-like interface for allocating and freeing DMA-capable
memory buffers. Designed for ease of use while remaining composable with
lower-level components.

pci.h
-----

Supports PCI device discovery, BDF parsing/formatting, and BAR mapping.
Useful for building user-space drivers or tools that interact with PCIe devices.

vfioctl.h
---------

Wraps Linux VFIO IOCTLs with convenience helpers and structs for managing
containers, IOMMU groups, and devices. Streamlines interaction with VFIO
from user space.

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
