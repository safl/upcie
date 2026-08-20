# Memory

Getting bytes to a device means answering one question: given a pointer, what
address does the device put on the bus? uPCIe has several answers, because the
right one depends on what the memory is and on what the kernel has been told
about it. This page is the map.

## The one abstraction

`struct dmamem` is a region of DMA-capable memory plus the rule for computing
device addresses within it. Everything above it, the NVMe request builders, the
queue-pair allocators, your own code, asks the same two questions and never
looks further down:

```c
uint64_t dmamem_va_to_iova(struct dmamem *dmem, void *vaddr);
uint64_t dmamem_offset_to_iova(struct dmamem *dmem, size_t offset);
```

"iova" is the name used throughout for the address the device sees, as DPDK
does. What it actually holds depends on the rule: a kernel-assigned IOVA when a
mapping was installed, a physical or bus address when none was.

Everything below `dmamem` is about filling one in.

Translation is the most visible part of the job and most of this page is about
it, but it is not the whole of it. A `dmamem` also says where the region is and
how large, whether the CPU can reach it at all, `cpu_va` is NULL for GPU memory
since VRAM is not CPU-mappable, what the memory is made of, and who is
responsible for tearing it down.

What the region description does not do is hand out memory. It describes a
region that already exists, and one `dmamem` typically serves many buffers. The
abstraction does provide allocation, but as a separate layer: `dmamem_heap`
takes a `dmamem` and carves it up, and each memory source offers a
malloc-shaped interface over its own heap. Keeping the two apart is what lets a
region be described once and allocated from by whatever policy suits, and it is
why a `dmamem` is not the thing you pass to an I/O call. That is a buffer, and
buffers come from the heap.

The `dmamem_` prefix therefore spans three kinds of thing, which is worth
knowing when scanning the headers: the region description itself
(`dmamem.h`), the constructors that fill one in (`dmamem_from_*`), and the
services layered over one (`dmamem_heap`). Only the first is a `dmamem`.

`dmamem` is uPCIe's own abstraction, not a kernel one. That is worth stating
plainly, because this page also talks about dma-buf, which is a Linux kernel
framework with its own documented semantics, and the two are easily taken for
the same kind of thing. `dmamem` belongs with `hostmem`, `cudamem` and
`hipmem`: each is a *memory source*, and `dmamem` is the unified view across
them, invented here to absorb the fact that the address a device needs
is computed differently depending on where the memory came from and on whether
anything installed a mapping.

Two words are used precisely throughout, and both are worth pinning down before
going further.

region
: What a single `dmamem` describes: one contiguous span of virtual addresses,
  starting at `base_va` and running for `size` bytes. Contiguous to *you*, that
  is. Whether it is also contiguous to the device is the question the
  translator answers. An address outside the span is not this `dmamem`'s
  business, and asking it to resolve one is a caller error rather than a
  lookup that fails.

granule
: The unit within which addresses are contiguous for the device, when nothing
  translates. A hugepage for host memory, an allocation chunk for GPU memory.
  It is the stride of the lookup table: one entry covers one granule, and
  addresses within a granule are reached by addition. Where a mapping *is*
  installed the notion does not apply, since the whole region is contiguous.

## Two enums, and the rule that connects them

A `dmamem` carries two classifications, and they are easy to conflate:

`backing` (`enum dmamem_backing`)
: Where the bytes live. `MEMFD`, `DMABUF`, `HOSTMEM`, `CUDAMEM`, `HIPMEM`.

`translator` (`enum dmamem_translator`)
: How an address is computed. `ARITHMETIC` is `base_iova + offset`. `LUT` is
  `phys_lut[offset >> shift] + (offset & mask)`.

They are not the same axis, and neither implies the other. Host hugepages are
`ARITHMETIC` when something installed a mapping for them and `LUT` when nothing
did. The rule is:

> The translator follows whether a mapping was installed, not what the memory
> is. If the kernel gave you a contiguous IOVA, address arithmetic is exact and
> free. If nothing translates, the device consumes physical addresses, the
> memory must be pinned and contiguous at some granule, and a table is needed
> to find each granule's address.

## The grid

Each constructor fills in one cell: a kind of memory, reached through a
particular kernel interface.

| Constructor | Backing | Translator | How the address is obtained |
|---|---|---|---|
| `dmamem_from_memfd()` | `MEMFD` | `ARITHMETIC` | hugepage-backed memfd mapped into an iommufd IOAS |
| `dmamem_from_dmabuf()` | `DMABUF` | `ARITHMETIC` | any exporter's dma-buf, including CUDA VMM and HIP, mapped into an iommufd IOAS via `IOMMU_IOAS_MAP_FILE` |
| `dmamem_from_hostmem_iommufd()` | `HOSTMEM` | `ARITHMETIC` | an existing hugepage mapped into an iommufd IOAS |
| `dmamem_from_hostmem_type1()` | `HOSTMEM` | `ARITHMETIC` | an existing hugepage mapped into a legacy vfio type1 container |
| `dmamem_from_hostmem_lut()` | `HOSTMEM` | `LUT` | nothing installed; physical addresses read per hugepage |
| `dmamem_from_cuda_lut()` | `CUDAMEM` | `LUT` | nothing installed; VRAM addresses enumerated by exporting the heap through `cuMemGetHandleForAddressRange()` as a dma-buf |
| `dmamem_from_hip_lut()` | `HIPMEM` | `LUT` | nothing installed; VRAM addresses enumerated by exporting the heap through `hipMemGetHandleForAddressRange()` as a dma-buf |

Several rows lean on dma-buf. That is a Linux kernel framework for sharing
buffers between drivers and out to user-space, documented in the kernel's own
[Buffer Sharing and
Synchronization](https://docs.kernel.org/driver-api/dma-buf.html); uPCIe's
`dmabuf.h` and `experimental/dmabuf_import.h` are convenience libraries over
the ioctls the kernel exposes for it, and add nothing of their own to the
model.

The two GPU rows differ in one number that matters downstream. The chunk a
single address lookup covers is the vendor's allocation granularity: typically
2 MiB on NVIDIA discrete GPUs, queried through
`cuMemGetAllocationGranularity()`, where each chunk's BAR1 window is contiguous
by the large-page guarantee; and the device page size on AMD, which is 4 KiB.
A table covering an address range therefore needs 512 times as many entries on
AMD as on NVIDIA for the same span.

Read the table as a grid and the shape of the library appears: the left half is
"what am I pointing at", the right half is "who, if anyone, is translating".

GPU memory appears twice in that grid. The `_lut` constructors are the case where nothing translates: the
device is handed VRAM addresses directly. `dmamem_from_dmabuf()` is the other
case, and it is not host-only: a dma-buf exported by the CUDA or HIP runtime
can be imported into an iommufd IOAS the same as any other exporter's, which
puts VRAM behind an IOMMU with an arithmetic translator and no address table
at all.

Whether that import succeeds depends on the running kernel rather than on
anything here, which is why the constructor is written as a probe: a failure
surfaces the exact errno so the block resolves to a concrete answer, kernel
side, exporter side or IOAS configuration. Upstream is moving toward accepting
dma-buf-backed fds through `IOMMU_IOAS_MAP_FILE`, and until that is widespread
the out-of-tree `iommu-map-pa` module in {doc}`libraries` takes the other
route: it maps an array of device-physical addresses, such as a CUDA-derived
`phys_lut`, into the IOMMU domain the VFIO-controlled device already uses and
returns an IOVA base to write into PRPs.

## Resolving one address

The rule a `dmamem` carries is small enough to write out. Worked examples, with
a buffer at `0x7f2c00403000` in a region based at `0x7f2c00400000`:

::::{tabs}

:::{tab} ARITHMETIC

Something installed a mapping, so the whole region has one contiguous device
address range and resolution is a single addition.

```
va       0x7f2c00403000
base_va  0x7f2c00400000   ---------------------------------.
                                                           |
offset = va - base_va                    = 0x3000          |
iova   = base_iova + offset                                |
       = 0x100000000 + 0x3000            = 0x100003000  <--'

  region  |<------------------ one IOVA range ------------------>|
   va     +--------+--------+--------+--------+--------+---------+
   iova   +--------+--------+--------+--------+--------+---------+
          ^ base_iova, then contiguous for the whole region
```

No table is consulted, and no boundary matters: the region is as contiguous to
the device as it is to you.
:::

:::{tab} LUT

Nothing translates, so the device is given physical addresses. Those are only
contiguous within a granule, one hugepage or one GPU allocation chunk, so each
granule's address is looked up and the offset within it added.

```
va       0x7f2c00403000
base_va  0x7f2c00400000
granule  2 MiB  ->  shift 21, mask 0x1fffff

offset = va - base_va          = 0x3000
index  = offset >> 21          = 0
within = offset & 0x1fffff     = 0x3000
iova   = phys_lut[0] + 0x3000

           phys_lut
   index   +---------------+
     0     | 0x00c0000000  |   <-- granule 0 lives here
     1     | 0x0180000000  |   <-- granule 1 somewhere else entirely
     2     | 0x00e0000000  |
           +---------------+

  va      |<-- granule 0 -->|<-- granule 1 -->|<-- granule 2 -->|
  phys      0x00c0000000      0x0180000000      0x00e0000000
```

Consecutive granules in the address space are unrelated in physical terms,
which is the whole reason the table exists.
:::

:::{tab} Crossing a granule

The consequence for building a command: within a granule, physical addresses
advance in step with virtual ones, so a PRP list is built by addition. Only at
a granule boundary is another lookup needed.

```
128 KiB read, 4 KiB pages, 2 MiB granule

  page  0   1   2   3  ...  31
        |   |   |   |        |
        v   v   v   v        v
  iova  P  P+1 P+2 P+3 ... P+31      (P from one lookup, then + 4096)

        |<------ all inside granule 0 ------>|

  a buffer straddling the boundary instead:

  page  ... 510 511 | 512 513 ...
                    |
        addition ---+--- fresh lookup, phys_lut[index + 1]
```

So a 128 KiB buffer inside a 2 MiB hugepage costs one lookup and 31 additions,
not 32 lookups. `nvme_request_prep_command_prps_contig_dmamem()` is written
this way.
:::

::::

As noted at the top, allocation sits *above* a `dmamem` rather than inside it.
`dmamem_heap` sub-allocates from one: `dmamem_heap_init()` takes a
`dmamem` and an alignment, then `dmamem_heap_alloc()` and friends hand out
pieces, with `dmamem_heap_at_va()` and `dmamem_heap_at_iova()` to go back and
forth. `hostmem_dma_malloc()` and `cudamem_dma_malloc()` are the malloc-shaped
conveniences each memory source offers over its own heap.

## Ownership

`dmamem` distinguishes memory it created from memory it merely describes, and
`dmamem_destroy()` behaves accordingly:

`owned = 1`
: The constructor allocated the fd and the mapping. Destroy unmaps and closes.
  This is what the `from_memfd` and `from_dmabuf` constructors produce.

`owned = 0`
: The `dmamem` wraps memory the caller owns, such as an existing hugepage or a
  GPU heap. Destroy removes only the mapping it installed, if any, and the
  caller's memory outlives it.

The wrapping constructors borrow rather than copy. A `LUT` translator points
`phys_lut` at a table someone else populated, so a `dmamem` that outlives that
table is a use-after-free on every translation, not a stale value.

## A read, end to end

Taking a 128 KiB read from a hugepage-backed buffer with no IOMMU, which is the
`dmamem_from_hostmem_lut()` cell:

1. `hostmem_hugepage_alloc()` reserves the pages and reads their physical
   addresses into a table, one per hugepage.
2. `dmamem_from_hostmem_lut()` wraps that as a `dmamem`: backing `HOSTMEM`,
   translator `LUT`, `phys_lut` borrowed from the hugepage.
3. `dmamem_heap_init()` sub-allocates the region; `dmamem_heap_alloc()` returns
   a buffer.
4. `nvme_request_prep_command_prps_contig_dmamem()` builds the command,
   resolving addresses as the *Crossing a granule* tab above describes.
5. The doorbell is rung; the device reads the PRPs and DMAs to those addresses.

Note where the work is. Steps 1 and 2 happen once per runtime, step 3 once per
buffer, and step 4 once per command, resolving per granule boundary rather than
per page. Nothing consults the kernel on the I/O path.

## Adding a memory source

A *memory source* is a module that allocates memory of one backing, discovers
the addresses a device needs for it, and can present itself as a `dmamem`.
`hostmem`, `cudamem` and `hipmem` are the three that exist. Adding a fourth,
OpenCL say, means writing the following, and reading the existing three is a
reasonable way to see the shape but a poor way to tell which parts are
essential.

Required:

`<x>mem_config`
: The page size and allocation granularity of the memory this source hands
  out. The granularity is not cosmetic: it becomes the granule, and therefore
  the stride of every lookup table built over this source.

`<x>mem_heap`
: Allocate a region and enumerate the device addresses behind it into a table,
  one entry per granule.

`<x>mem_as_dmamem()`
: The bridge. Fills in a `dmamem` that borrows the heap's table, so `backing`
  is this source's and `translator` is whatever suits it.

a value in `enum dmamem_backing`
: So a `dmamem` can say what it is.

Optional, present in some sources and not others:

`<x>mem_dma`
: A malloc-shaped interface over the heap. `hostmem` and `cudamem` have one.

`<x>mem_mapping`
: A registry for buffers the caller allocated rather than the heap. `cudamem`
  and `hipmem` have one; `hostmem` does not yet, which is a gap rather than a
  decision. A caller with their own hugepage arena cannot register it today,
  and has to copy into a uPCIe-allocated buffer instead.

Two invariants a new source can violate quietly, both of which produce wrong
addresses rather than failures:

- The memory must be pinned, and contiguous in device-address terms across the
  granularity the config declares. One table entry per granule is only correct
  if the granule really is contiguous.
- The source keeps ownership. `_as_dmamem()` produces a view, so the heap and
  its table must outlive every `dmamem` borrowing them.

## What is not covered

The model above assumes each region is one contiguous range, anchored at
`base_va`, with addresses resolved as an offset from it. Memory the application
allocated and handed over, rather than allocated from a uPCIe heap, does not
fit that shape: it lives elsewhere in the address space, and there may be many
such regions at once. Supporting it means resolving an absolute address across
a set of regions rather than an offset within one.

Note which half of the grid that gap belongs to. Where an IOMMU translates it
does not arise: importing the caller's memory through
`dmamem_from_dmabuf()` yields one contiguous IOVA for the whole range, so it
is an ordinary `ARITHMETIC` region and there is nothing further to solve.
The gap is specific to the untranslated half, where the device consumes
physical addresses and each region needs its own table of them. So this is a
question about the `LUT` column rather than about registration in general, and
it shrinks as `IOMMU_IOAS_MAP_FILE` acceptance of dma-buf-backed fds becomes
something one can rely on.
