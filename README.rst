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