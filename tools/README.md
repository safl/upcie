# Probing tools

Small standalone binaries that report what a runtime or the kernel actually
does. They differ from `tests/` in intent: a test asserts that behaviour is
what uPCIe requires, a probe finds out what the behaviour is. Probes print and
exit zero; they do not fail when the answer is inconvenient.

They build with the rest of the project, each only where its dependency is
present, so the CUDA probe appears on a machine with the CUDA driver and the
HIP one on a machine with ROCm.

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
