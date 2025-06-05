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
-----

So far, then two tools are materializing:

vfioctl.h
  A header-only library which extents the Linux **UAPI** for ``VFIO`` **ioctl**
  with helper-functions wrapping the various **ioctl** along with structs for
  encapsulating containers, groups, and devices.

devbind
  A utility to list PCIe devices and their current driver association,
  along with functionality to unbind and bind drivers. This is currently a
  self-contained Python script, the intent is that this script should turn into
  something that can replace the ``xnvme-driver`` script.

I would expect a header-only equivalent for ``uio-pci`` would be added, along
with a minimal memory-allocator based on ``HUGEPAGES``. And possibly other
things as well. Also, this should also be investigated for FreeBSD.

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
