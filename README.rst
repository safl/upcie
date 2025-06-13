uPCIe: notes on exploration of BAR-space-mapping, and DMA-able memory
=====================================================================

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

Tools
=====

So far, tools described in the following subsections have materialized. The
**C** tools are:

- **Header-only**
- **Idiomatic**
- **Zero-dependency**
- **C99/C11** compatible

These are intended as small, drop-in components for **C** projects, providing
convenient access to the functionality they encapsulate.

The **devbind** tool is a Python-based CLI utility. It is designed to be usable
as a self-contained, standalone script that you can download and run with a
reasonably modern Python interpreter on your system.

vfioctl.h
---------

An extension of the Linux **UAPI** for ``VFIO``, providing helper functions that
wrap the various **IOCTLs**, along with structs for encapsulating containers,
groups, and devices.

mmio.h
------

A library providing helpers for 32-bit and 64-bit reads and writes to
memory-mapped I/O regions. These functions use volatile access to ensure correct
ordering and visibility, making them suitable for device register access through
PCI BAR mappings. Ideal for user-space drivers or prototyping with VFIO or UIO.

hostmem.h
---------

A helper that provides a hugepage allocator, along with a malloc-like buffer
allocator built on top of it. The result is convenient allocation of
DMA-capable buffers.

pci.h
-----

A collection of functions to: "scan" for PCI devices, obtain handles to them,
and map their BAR regions. Includes helpers for converting between textual and
structured BDF representations.

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

Planned Additions
-----------------

A header-only equivalent for ``uio-pci`` is expected to be added, along with a
minimal memory allocator based on **hugepages**. Additional tools may follow.
There is also an interest in investigating support for FreeBSD.


Host Memory for DMA
-------------------

When using host memory for DMA -- such as in user-space drivers -- the following
memory attributes are desirable:

Pinned
  A region of memory is dedicated (i.e., "pinned") for DMA purposes. Ensuring
  that pages within the region cannot be swapped to disk, reallocated, or
  moved in any way. The memory is effectively reserved for a specific use case,
  guaranteeing availability when a peripheral (e.g., an NVMe device or GPU)
  needs direct access to host memory.
  
Contigous
  ..

DMA Address Mapping
~~~~~~~~~~~~~~~~~~~

At the host level, each process has an isolated virtual address space, which
simplifies host software development. However, addresses in this space are not
accessible to peripheral devices. Data must be transferred using methods such as
Direct Memory Access (DMA), performed by an on-device DMA engine, which requires
a DMA-addressable location -- either a physical address or an IO Virtual Address
(IOVA).

Thus, at the host level, one maintains a mapping from the isolated virtual
address space to either **PHYS** or **IOVA** addresses. The DMA engine do not
care what the address is, it will just do DMA with it.

Note that IOVAs require translation by an IOMMU, known as the
address-translation-service (**ATS**), which maps IOVAs to physical addresses.
Without an address-translation-cache (**ATC**), address-translation can become
a bottleneck.

So when does host software use this? One example is when writing a user-space
NVMe driver and constructing an NVMe command. Fields such as the data pointer,
``PRP1``, ``PRP2``, ``PRP``-lists, or ``SGL```-lists require DMA-capable
addresses. Therefore, the user-space driver must determine what the
process-isolated virtual address maps to -- either a **PHYS** or **IOVA** -- to
ensure the command uses a valid DMA address.

IPC via Shared Memory
~~~~~~~~~~~~~~~~~~~~~

One interesting side-effect of using e.g. pinned hugepages user-space, is that,
since memory-addressing is physical, then processes can communicate with each
other (Inter Process Commmunication -- IPC), without any multi-processing,
threading, locking, or message-passing. They can simply by using the same
physical memory.

However, this is by default not possible unless the memory is being mapped into
the process in a form where it is flagged for sharing. Thus, IPC via shared
memory can be obtained via 

System Setup
============

Ensure drivers are loaded::

  sudo modprobe vfio-pci
  sudo modprobe uio_pci_generic

Stuff about the IOMMU enabled/disabled.
Binding drivers.