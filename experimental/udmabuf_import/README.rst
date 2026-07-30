UDMABUF Import
==============

A *dma-buf* importer for *udmabuf*: it imports an external *dma-buf* and shares
its DMA/physical addresses with userspace over three ioctls (``UDMABUF_ATTACH``,
``UDMABUF_DETACH``, ``UDMABUF_GET_MAP``). Two examples import a *dma-buf* and
print its addresses: ``udmabuf_import_cpu`` (a *memfd* via *udmabuf*) and
``udmabuf_import_gpu`` (GPU memory via the NVIDIA driver).

Why DKMS rather than the in-tree patch: the patch bakes into ``vmlinuz``, so
every change means a full kernel rebuild and reboot (or kexec stunts). As a
standalone out-of-tree module it iterates fast (``rmmod``/``insmod``, or a DKMS
rebuild, to try a change), and it works in netboot environments where the kernel
is fixed.

Tested with the NVIDIA driver, CUDA runtime, and libraries: the GPU example
imports a CUDA-exported *dma-buf* through ``/dev/udmabuf_import`` and reads back
its DMA addresses.

Install the DKMS module
-----------------------

Stock ``/dev/udmabuf`` keeps serving ``UDMABUF_CREATE``; the module adds
``/dev/udmabuf_import`` for ``UDMABUF_ATTACH`` / ``DETACH`` / ``GET_MAP``.

* Install the ``.deb`` (CI builds it for Ubuntu 24.04 and 26.04)::

	  apt install ./udmabuf-import-dkms_*.deb

* Or build the ``.deb`` from this repo::

	  apt install build-essential debhelper dh-dkms
	  dpkg-buildpackage -us -uc -b

DKMS rebuilds the module automatically on kernel updates. Load it now with
``modprobe udmabuf_import`` (installing the ``.deb`` does not auto-load it). To
load it on every boot, create a ``.conf`` under ``/etc/modules-load.d/`` that
names the module, which ``systemd-modules-load`` reads at boot::

	  echo udmabuf_import > /etc/modules-load.d/udmabuf_import.conf

The package also installs the UAPI header at
``/usr/include/linux/udmabuf_import.h``, so userspace can build against the
import ioctls without vendoring::

	  #include <linux/udmabuf_import.h>

It provides the attach/detach/get-map structs and ioctls plus an overridable
``UDMABUF_IMPORT_DEVPATH`` (the device to open for the import ioctls).

Build and run the examples
--------------------------

* ``make cpu`` builds ``udmabuf_import_cpu`` (*udmabuf* + *memfd*)
* ``make gpu`` builds ``udmabuf_import_gpu`` (needs CUDA; links ``-lcuda``)

*dma-buf* is fd-backed, so raise the limit first: ``ulimit -n 1000000``.

Limitations and errors
----------------------

This section describes the limitations discovered while using the dma-buf
interface and the errors associated with these.

File descriptor limits
^^^^^^^^^^^^^^^^^^^^^^

*dma-buf* is based on file descriptors and every new dma-buf FD will count as
opening a file. Thus, you can easily run into errno 24 "EMFILE 24 Too many open
files". To avoid this, you can increase your user limits. You can temporarily
increase them with::

	ulimit -n 1000000

To permanently increase it, add the following lines to `/etc/security/
limits.conf`::

	<username> - nofile 100000

You can use '*' to set the limit for all users. However, this will not apply to
the root user. In this case you need to add a separat line where <username> is
root.

The value chosen above is arbitrary, if you are interested to see the system wide limit run::

	cat /proc/sys/fs/file-max

GPU memory limitations
^^^^^^^^^^^^^^^^^^^^^^

NVIDIA GPUs have two memory regions that we care about. One is called the
frame buffer (FB) memory, this memory is what we typically consider device/GPU
memory. We use this memory when making memory allocations on the GPU by running
`cudaMalloc`, `cuMemAlloc` or similar. The second region is the BAR1 memory.
This is used to map the FB memory allowing it to be directly accessed by the CPU
or through P2P DMA Transfers. Note, both regions might have a portion of memory
reserved for the GPU driver.

The sizes of the memory regions can be found by running the following::

	nvidia-smi -q -d memory

The output of this command should look something like this::

	...
	FB Memory Usage
        Total                             : 6138 MiB
        Reserved                          : 330 MiB
        Used                              : 0 MiB
        Free                              : 5809 MiB
    BAR1 Memory Usage
        Total                             : 8192 MiB
        Used                              : 1 MiB
        Free                              : 8191 MiB
	...

If we run out of memory from either region the execution will fail. FB memory
runs out if we make too large or too many allocations. The BAR1 memory runs out
if we make too large or too many mappings. This means that we might have many
allocations with a total size below the FB limit, but the size of the mappings
exceeds the available BAR1 memory. This is true both when using the NVIDIA kernel
P2P API or the NVIDIA Driver API to create the mappings.

The size of the mapping scales with the size of the allocation. On one system we
saw an 140KiB allocation take up 2MiB of BAR1 memory, while an 8MiB allocation
took up 8MiB of BAR1. We saw that it is possible to run out of BAR1 memory
before FB memory by creating a large amount of small (e.g., 140KiB) allocations
and mapping them. This made the program crash and `dmesg` showed the following
errors::

	NVRM: dmaAllocMapping_GM107: can't alloc VA space for mapping.
	NVRM: nvAssertOkFailedNoLog: Assertion failed: Out of memory [NV_ERR_NO_MEMORY] (0x00000051)

A different problem arises with single large allocations. On one GPU (NVIDIA RTX
A2000) we found a threshold of 1048560 * 4KiB, if more than this is allocated,
the IO CUDA Kernels runs forever. However, with multiple allocations we can
exceed this amount without problems. It is unclear why. We have not been able to
reproduce this with other GPUs.
