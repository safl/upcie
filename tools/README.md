# Probing tools

Small standalone binaries that report what a runtime or the kernel actually
does. They differ from `tests/` in intent: a test asserts that behaviour is
what uPCIe requires, a probe finds out what the behaviour is. Probes print and
exit zero; they do not fail when the answer is inconvenient.

They build with the rest of the project, each only where its dependency is
present, so the CUDA probe appears on a machine with the CUDA driver and the
HIP one on a machine with ROCm.

## upcie_vfio_delegate_probe

Asks two questions a design for sharing a controller has to answer, neither of
which is about GPUs.

**Which way registration goes.** A secondary holding the iommufd maps its own
memory, which pins pages against the secondary. A secondary that instead sends
a descriptor and asks the primary to map it pins them against the primary. The
probe does both, in that order, and prints `RLIMIT_MEMLOCK` on each side, so
running either side under a lowered limit shows which one it bounds.

**What survives the primary.** The primary exits after serving. The secondary
then re-reads a register through its own mapping of BAR0 and tries a further
mapping through the iommufd it was handed. A device fd keeps the device bound;
whether the address space outlives the process that created it is what a
restart policy turns on.

Usage, primary in the background since it serves one secondary and exits:

    upcie_vfio_delegate_probe primary /dev/vfio/devices/vfio8 /tmp/d.sock &
    upcie_vfio_delegate_probe secondary /tmp/d.sock
    sh -c 'ulimit -l 64; upcie_vfio_delegate_probe secondary /tmp/d.sock'

### Findings

Measured 2026-08-24 on Linux 7.0.0-28-generic, a Samsung NVMe controller bound
to `vfio-pci`, 2 MiB per mapping.

**RLIMIT_MEMLOCK bounds none of it.** With the limit at 64 KiB on the
secondary, and separately on the primary, every mapping still succeeded: the
secondary's own `MAP_FILE` of a memfd, its `IOMMU_IOAS_MAP` of anonymous
memory, and the primary's `MAP_FILE` on its behalf. So which side is "charged"
is not a question this limit answers, and an unprivileged secondary does not
need its limit raised to register memory.

**The address space outlives the primary.** After the primary exits, closing
its device fd and its iommufd, the secondary still reads `CAP` through its own
mapping of BAR0 and still maps new memory through the iommufd it was handed:

    [secondary] primary is gone                ok
    [secondary] host read after                0x28033fff
    [secondary] map after, via passed fd       ok, iova=0x800000

The descriptors a secondary holds keep both the device and the IOAS alive, so a
secondary is not obliged to die with the process that handed them over.

**Both registration directions work.** The secondary can map its own memory
through the passed iommufd, and the primary can map a descriptor the secondary
sends. Nothing in the kernel prefers one; the choice is a design decision, and
the accounting argument for one of them does not survive the first finding
above.

## upcie_vfio_cdev_probe

Asks whether a second process can claim a device another process already holds,
which is the question any design for sharing a controller has to answer first.

    upcie_vfio_cdev_probe <cdev> <attach:0|1> <hold-seconds>

Run it while a primary holds the device and works normally. `open()` on the
character device succeeds, which is what makes an independent second path look
feasible, and that is as far as it gets:

    --- SECOND, bind only:
      open(cdev)                   ok
      BIND_IOMMUFD                 Invalid argument
      GET_REGION_INFO(BAR0)        Invalid argument

A device cannot be bound to more than one `iommufd_ctx`, so
`VFIO_DEVICE_BIND_IOMMUFD` fails, and since the UAPI header states the user is
restricted from accessing the device before binding completes, everything after
it fails too. No region info means no BAR, which means no doorbell. Offering a
different iommufd does not help and neither does declining to attach.

Note what does succeed in that second process: opening `/dev/iommu`, allocating
an IOAS, and mapping memory into it. That is the trap, because the result is a
valid-looking address space attached to no device.

## upcie_vfio_share_probe

Asks whether a privileged primary can delegate a device to an unprivileged
secondary, and how far the delegation reaches. This is the same question
`upcie_vfio_delegate_probe` asks about accounting and lifetime, put instead
about privilege.

    upcie_vfio_share_probe primary   <cdev> <sock>
    upcie_vfio_share_probe secondary <sock>

The primary binds the device, maps a memfd into its IOAS, then passes the
device fd, the iommufd and the memfd over a unix socket. The secondary must be
run as an ordinary user for the answer to mean anything, since dropping root
inside one process is not the same test.

    [secondary uid=1000]
      recv fds                       ok
      GET_REGION_INFO(BAR0)          ok
      mmap(BAR0)                     ok
      NVMe CAP low dword             0x28033fff   (secondary)
      shared DMA buffer              wrote 0x00c0ffee
      IOAS_MAP own buffer            ok
        -> iova                      0x400000

Three things follow. The secondary reaches the BAR and therefore the doorbells.
It shares DMA memory through a memfd neither process had to name in the
filesystem. And it registers memory of its own choosing through the passed
iommufd, so it is not restricted to buffers the primary arranged in advance.
Reading the same `CAP` value the primary reads is what confirms it is genuinely
looking at the controller's registers.

## upcie_vram_ioas_probe_{cuda,hip}

Asks whether GPU memory can enter an iommufd IOAS, which is what a controller
behind an IOMMU would have to DMA against.

Under `uio_pci_generic` a controller consumes physical addresses and reaches a
GPU allocation through its dma-buf scatter list. Under `vfio-pci` it consumes
IOVAs, so the allocation has to be mapped with `IOMMU_IOAS_MAP_FILE`.
`iommufd.h` records that as of 6.19 that call accepts only dma-bufs exported by
vfio-pci and rejects those exported by CUDA or HIP. The probe asks the running
kernel rather than trusting the comment, and maps a `memfd` first as a control,
since a `MAP_FILE` that refuses everything would say nothing about GPU memory in
particular.

The question it was written to settle: can a controller under an IOMMU DMA into
VRAM at all? That sits upstream of every question about sharing one controller
between processes, because it decides whether GPU consumers can use `vfio-pci`
in the first place.

Usage: `upcie_vram_ioas_probe_cuda`, no arguments.

## upcie_vfio_share_gpu_probe_cuda

Asks whether a process that holds nothing but a passed vfio device fd can reach
the BAR from a GPU kernel.

Passing the descriptor over `SCM_RIGHTS` and mapping BAR0 in the receiver is
known to work. What that leaves untested is the step the GPU-initiated NVMe
path depends on: `cuMemHostRegister()` with `CU_MEMHOSTREGISTER_IOMEMORY` on a
window of the received mapping, and an SM reaching it. The read comes from a
kernel rather than from `cuMemcpyDtoH()`, because the copy engine is not what
rings a doorbell.

It is two processes rather than a fork with a `setuid()` in it, because
dropping privilege is not the same state as never having had it: the dropped
process keeps the supplementary groups it started with unless they are cleared,
and a uid change clears the dumpable flag, either of which could be what a
refusal is really about. So the secondary is started separately, as whatever
user starts it, and the two meet over a named socket. It reads `CAP` rather
than writing a doorbell, so it disturbs nothing, and both sides print what they
read.

Usage, with the primary in the background since it serves one secondary and
leaves when it disconnects:

    upcie_vfio_share_gpu_probe_cuda primary /dev/vfio/devices/vfio8 /tmp/probe.sock &
    setpriv --reuid=1000 --regid=1000 --clear-groups \
        upcie_vfio_share_gpu_probe_cuda secondary /tmp/probe.sock

### Findings

Measured 2026-08-24 on Linux 7.0.0-28-generic, NVIDIA RTX A6000 with driver
580.173.02 and CUDA 13.3, against a Samsung NVMe controller bound to
`vfio-pci`, reading `CAP` at BAR0 offset 0. Two runs against the same primary,
differing only in who starts the secondary.

A passed descriptor is enough, and privilege decides how far it goes. Started
as root, the secondary gets all the way, and the SM reads what the primary
reads:

    [secondary] running as                   uid=0 euid=0 gid=0
    [secondary] recv device fd               ok
    [secondary] host read                    0x28033fff 0x08000030
    [secondary] cuMemHostRegister(IOMEMORY)  ok
    [secondary] kernel read                  0x28033fff 0x08000030

Started as uid 1000, with `setpriv --reuid=1000 --regid=1000 --clear-groups` so
that it never held privilege and carries no inherited groups, it receives the
descriptor, maps BAR0 and reads the same register, then stops:

    [secondary] running as                   uid=1000 euid=1000 gid=1000
    [secondary] cuMemHostRegister(IOMEMORY)  CUDA_ERROR_NOT_PERMITTED

The `standalone` mode is the control for that, opening the device itself with
no delegation anywhere. With the cdev and `/dev/iommu` chowned to the user, an
unprivileged process binds, attaches, maps BAR0 and reads the same register,
and is refused in the same place:

    [standalone] running as                   uid=1000 euid=1000 gid=1000
    [standalone] bind and attach              ok
    [standalone] host read                    0x28033fff 0x08000030
    [standalone] cuMemHostRegister(IOMEMORY)  CUDA_ERROR_NOT_PERMITTED

while the same binary as root reads `CAP` from a kernel. So the descriptor
delegates the device, and `CU_MEMHOSTREGISTER_IOMEMORY` delegates nothing: it
wants privilege of the calling process, and passing a descriptor has nothing to
do with it. An unprivileged process can drive a controller from the CPU, its
own or a delegated one, since ringing a doorbell from the host is an ordinary
store into a mapping it already has. It cannot submit from a GPU kernel either
way. Which capability short of root suffices was not tested.

Worth noting in passing, since it is the model libvirt uses and it is now
measured here: an unprivileged process can own a vfio device end to end when
udev gives it the nodes.

## upcie_vfio_bar_import_probe_{cuda,hip}

Asks whether a GPU runtime will import another device's MMIO as a dma-buf and
hand back a device pointer.

`VFIO_DEVICE_FEATURE_DMA_BUF` exports a slice of a BAR as a dma-buf, and that
descriptor has no CPU mapping: `test_dmamem_vfio_bar` reports
`cpu_va=unavailable`. So a process holding only the descriptor cannot produce a
host address, and `cuMemHostRegister()` with `CU_MEMHOSTREGISTER_IOMEMORY`,
which is how the GPU-initiated NVMe path reaches a doorbell, has nothing to
register. The remaining route is for the runtime to import the descriptor
itself.

The probe binds the device, exports a slice, imports it through
`cuImportExternalMemory` or `hipImportExternalMemory`, maps it, and reads the
first two dwords back from the device side. It reads rather than writes and
defaults to offset 0, so what it touches is `CAP` and not a doorbell; the host
reads the same register through its own mapping and the two are printed side by
side. Equal values are the whole result.

The question it was written to settle: can MMIO be delegated to a process that
never holds the device fd? If the import fails, delegating a controller's
doorbells means delegating the controller.

Usage: `upcie_vfio_bar_import_probe_cuda <cdev> [region] [offset] [length]`,
for example `upcie_vfio_bar_import_probe_cuda /dev/vfio/devices/vfio8 0 0
0x1000`.

### Findings

Measured 2026-08-24, both hosts Linux 7.0.0-28-generic, against a Samsung NVMe
controller bound to `vfio-pci`, BAR0 at offset 0 for 4 KiB. warp: NVIDIA RTX
A6000, driver 580.173.02 (the open modules, `Dual MIT/GPL`), CUDA 13.3. wave:
AMD Radeon RX 7800 XT with ROCm.

Neither runtime takes it, and on NVIDIA the reason is not the one the probe was
written to find:

    CUDA   self-import (control)  CUDA_ERROR_NOT_SUPPORTED(801)
           import(DMABUF_FD)      CUDA_ERROR_NOT_SUPPORTED(801)
           import(OPAQUE_FD)      CUDA_ERROR_UNKNOWN(999)
    HIP    hipImportExternalMemory=hipErrorOutOfMemory(2)

The control allocates GPU memory, exports it with
`cuMemGetHandleForAddressRange`, and imports that back. It fails identically, so
`cuImportExternalMemory` with `CU_EXTERNAL_MEMORY_HANDLE_TYPE_DMABUF_FD` is
unsupported on this stack for any exporter, and says nothing about vfio. The
export direction works; only the import is refused. Nothing appears in the
kernel log while it happens, and the open modules' importer,
`nv_dma_import_from_fd()` in `nvidia/nv-dmabuf.c`, prints on both of its failure
paths, which suggests the refusal is above the open layer.

HIP has no dma-buf handle type at all: `hipExternalMemoryHandleType` stops at
`NvSciBuf`, so `OpaqueFd` was the only thing to ask for.

The export side is sound in both cases: `EXPORT_DMA_BUF` succeeds, and the host
reads `0x28033fff` for `CAP` through its own mapping.

So MMIO cannot be handed to a process as a descriptor today. A process that
must ring a doorbell has to hold the device fd, map the BAR itself, and register
the host address with `cuMemHostRegister(CU_MEMHOSTREGISTER_IOMEMORY)`, which is
what the GPU-initiated path already does.

## upcie_dmabuf_probe_{cuda,hip}

Asks what a GPU runtime returns when told to export a device address range as
a dma-buf, which is the mechanism uPCIe translates VRAM addresses through.
Needs the out-of-tree `dmabuf-import` module from `experimental/`, since
reading the scatter list of an imported dma-buf is not something mainline
exposes to userspace.

Per range it prints the segment count, how many bytes those segments describe
against how many were requested, the shortest segment, the number of maximal
physically contiguous runs, and the largest power-of-two granule the list can
be indexed by. Then it asks whether the allocation a pointer falls in can be
recovered from the pointer.

The questions it was written to settle:

1. Does the export describe the requested range, or something else?
2. Is a sub-range at a non-zero offset described from that offset?
3. What granule is the memory really contiguous at, as opposed to the
   `alloc_granularity` the runtime reports?
4. Can a registration recover the allocation a caller's pointer belongs to?

### Findings

Measured 2026-08-20, both hosts Ubuntu 26.04, Linux 7.0.0-28-generic.
NVIDIA RTX A6000, driver 580.173.02, CUDA driver API 13000. AMD Radeon RX 7800
XT (Navi 32), HIP driver 70152801. Two 64 MiB allocations per run.

The `alloc_granularity` the runtimes report, 2 MiB on NVIDIA and 4096 on AMD,
describes neither the export nor the physical layout.

**CUDA honours the request.** A 4 KiB export describes exactly 4096 bytes; a
2 MiB export exactly 2 MiB; a 3 MiB export exactly 3 MiB. An export at
`base + 4K` starts at the physical address of `base + 4K`, not of `base`.
Segments come back at `device_pagesize`, 64 KiB, so the whole allocation
arrives as 1024 of them, but the addresses are adjacent: every range probed was
a single contiguous physical run, the full 64 MiB included.

**AMD ignores the request.** Every export, 4 KiB through 64 MiB, at base or at
any offset, returned the same scatter list describing the whole underlying
buffer object. Asked for 4096 bytes, it described 67108864, and the first
segment was the base of the allocation whatever offset was asked for. The two
allocations produced different lists, 6 runs and 16, so the export is
per-allocation. Every run was a multiple of 4 MiB, and the shortest segment
seen was 4 MiB.

So `hipMemGetHandleForAddressRange` is, on this stack, a whole-object export
whose range arguments are accepted and discarded.

**An odd-sized allocation is reported unrounded, but backed differently.**
Asked for 3 MiB + 4 KiB, both runtimes reported the size back as exactly
3149824. CUDA's export described exactly that many bytes, ending in a 4 KiB
segment; AMD's described 4194304, the whole 4 MiB object, which is more than
the size it had just reported. So the size a runtime reports bounds neither the
export below nor above in general.

**Both recover the allocation from a pointer.** `cuMemGetAddressRange` and
`hipMemGetAddressRange` returned the exact base and size for a pointer at the
base, at `base + 3M + 4K`, and at the last byte.

### What this means for translation

Exporting once per chunk of a registered region cannot work on AMD, because
each chunk export re-exports the entire object. The export has to happen once
per allocation, with the scatter list indexed by offset from the allocation
base, and the base recovered with `hipMemGetAddressRange` rather than assumed
to be the pointer the caller passed. That shape is correct on CUDA too, which
makes it the one to build.

Getting it wrong is silent rather than loud. A caller registering at
`base + 2M` and trusting the first segment address receives the physical
address of `base`, and the DMA lands 2 MiB away from where it should. Checking
the described length against the requested length, as `dmabuf_get_lut()` does,
turns that into `-EINVAL` instead, which is the only reason this surfaced as an
error rather than as corruption.

A 2 MiB translation granule is supported on both, and for different reasons:
on CUDA because the allocations were contiguous well past 2 MiB, on AMD because
no physical run was shorter than 4 MiB. It is worth noting what that rests on,
which is two allocations on one part per vendor. It also does not by itself fix
anything on AMD, since a 2 MiB request at a 2 MiB offset is discarded like any
other; the fix is the per-allocation export, and the granule is a separate
choice on top of it.

It also means a LUT at a chosen granule cannot be filled by asking
`dmabuf_get_lut()` for `ceil(size / granule)` entries, since that is a length
mismatch on CUDA whenever the allocation does not end on a granule boundary.
Walking the scatter list directly handles the short final granule, and lets the
walk verify contiguity within each granule as it goes, which is what turns a
whole-object mismatch into an error rather than a wrong address.

Nor does a granule have to become an alignment requirement on callers. Once the
allocation base is recovered, the offset of a caller's pointer within it is
known exactly, so registration can accept any pointer the vendor recognises and
leave alignment to what NVMe PRP construction needs.
