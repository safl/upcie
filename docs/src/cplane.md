# Control plane

A vfio device file cannot be bound twice. So a process that wants a controller
another process already has open cannot open it again; it has to be handed the
descriptor. A shared memory segment cannot carry one. A unix socket can, and
that is what the control plane is.

What travels over it is the control plane in the sense of the split between
control and data: everything about a controller that is done once for all of
its users, its admin queue, its queue identifiers and the memory the device
reaches, as opposed to the data plane, the I/O itself. One process opens the
controllers and holds the control plane. Others connect and are given what
they cannot take for themselves: the memory, the queues, and the admin
queue's answers. What they are not given is a detour for their I/O, which goes
from the client to the hardware without the server in it.

## Who owns what

```{mermaid}
flowchart LR
    subgraph server ["Server process"]
        heap["DMA heap<br/>allocator, free list"]
        aq["Admin queue"]
        qid["Queue identifiers"]
    end

    subgraph client ["Client process"]
        ioq["I/O queues<br/>its own doorbells"]
        map["Its mapping of<br/>heap and BAR0"]
    end

    ctrl(["NVMe controller"])

    client -- "requests over a socket" --> server
    server -- "descriptors, offsets" --> client
    server -- "admin commands" --> ctrl
    client -- "I/O, no server in the path" --> ctrl
```

The server owns everything that has to be owned once: the heap allocator and
its unlocked free list, the identifier space for queues, and the admin queue.
A client owns its mapping of the heap and of BAR0, the queues it was given,
and the requests it builds in memory it was allocated.

## Rendezvous

A server listens on one unix socket, whatever it holds, named for the
identifier it serves under. A client derives the same name and connects, and
names the controller it wants in each request.

A client need not know what a server holds before it asks, and a process
using several controllers needs one connection; naming the controller in the
message costs four bytes.

The socket is a filesystem path rather than an abstract address, because an
abstract address cannot be reached from another network namespace and a
containerised client would need to.

## The message

One fixed-size message travels in both directions, request and reply sharing a
layout, on a `SOCK_SEQPACKET` socket. Every message is one packet, so there is
no framing, a message and the descriptors it carries arrive together or not
at all, and a read of nothing means the peer is gone.

| Field     | Meaning                                          |
| --------- | ------------------------------------------------ |
| `op`      | One of `enum nvme_cplane_op`                     |
| `version` | `NVME_CPLANE_VERSION` as the sender knows it     |
| `result`  | Replies only: zero, or a negative errno          |
| `nfds`    | Descriptors accompanying the message             |
| `index`   | Which of the server's controllers this is about  |
| `u`       | Per-operation payload                            |

The payload of every operation is a struct of its own, such as
`nvme_cplane_init_connection` and `nvme_cplane_alloc_ioqpair`, each exactly
`NVME_CPLANE_PAYLOAD_NBYTES`, so the message is the same size whatever it
carries.

`index` runs from zero, and a status reply says how many there are, so a
client walks the range to find the controller it wants and matches on the
identifier the server publishes for it.

The version is checked in both directions. The messages describe queue memory
whose layout comes from this library, so a peer built against another version
cannot be trusted to read it and is refused rather than served. The version is
the protocol's, in `upcie_versions.h`, and not the library's: two releases
that agree on it work together.

## Initialising a connection

Initialising is the only exchange whose reply carries descriptors. The client
receives the DMA heap and the device, maps both, and builds its own controller
around the `struct nvme_controller_base` the reply carries: what the
controller was opened with. It is sent once per controller a
connection intends to use: the heap is the runtime's and comes back every
time, so a client that has it already closes that descriptor and keeps what it
has, while BAR0 is the controller's own.

```{mermaid}
sequenceDiagram
    participant C as Client
    participant S as Server
    participant K as Controller

    Note over S,K: controller open,<br/>heap description written

    C->>S: connect to the runtime's socket
    C->>S: STATUS(index) until the controller is found
    C->>S: INIT_CONNECTION(index)
    S-->>C: heap fd, device fd<br/>controller base, desc_offset, heap_nbytes, bar0_nbytes
    C->>C: map the heap, map BAR0
    C->>C: check the base against BAR0
    C->>C: build its own controller and request pool
```

Nothing is rebased and no pointer written by one process is read by another.
What travels is a description plus offsets, and the receiving process resolves
them against its own mapping. The heap description carries the physical
address of each granule, because the server read those when it allocated,
which needs `CAP_SYS_ADMIN`, and a client that cannot read pagemap takes them
from where they were left.

## Asking for things

Once initialised, a client submits I/O by itself. Everything else is a
request, because the resource belongs to the server.

```{mermaid}
sequenceDiagram
    participant C as Client
    participant S as Server
    participant K as Controller

    C->>S: ALLOC_IOQPAIR (depth)
    S->>S: take SQ, CQ and PRP memory from the heap
    S->>K: Create CQ, Create SQ
    K-->>S: completions
    S-->>C: queue id and the three offsets
    C->>K: I/O, ringing its own doorbell

    C->>S: REGISTER_MEM (dma-buf fd, nbytes, page_size)
    S-->>C: heap offset of the region's description

    C->>S: ALLOC_IOQPAIR_AT (depth, registered region, SQ and CQ offsets)
    S->>S: take what stays in the heap from the heap
    S->>K: Create CQ and SQ, each where it was placed
    S-->>C: queue id and the three offsets
    C->>K: I/O on the queues where they were placed

    C->>S: ALLOC_BUF (nbytes)
    S-->>C: offset into the heap

    C->>S: ADMIN_CMD (command)
    S->>K: submitted on the admin queue
    K-->>S: completion
    S-->>C: completion

    C->>S: FREE_IOQPAIR / FREE_BUF
    S-->>C: status
```

A client with memory of its own can also have either queue, or both, placed in
a region it registered; one left to the heap is named
`NVME_CPLANE_QUEUE_IN_HEAP`. A GPU submitting I/O itself places both. A host
doing P2P places only the completion queue, so the controller completes beside
the client's data rather than across PCIe into host memory, while the host
submits from the heap as it always did. The offsets that come back name heap
queues as for any allocation, and one standing in for a placed queue is never
used by the controller; a host client keeps the heap completion queue a copy
of the one it placed, so that completions are read the same way whichever
queue they land in.

An admin command's payload does not travel with it. The command names an
address the device can already reach, from memory the client was allocated, so
an identify lands in the client's own buffer and only the command and its
completion cross.

## Many clients at once

The expected arrangement is several client processes at the same time, each
allocating buffers, creating queues and submitting admin commands, none of
them synchronising with any other. Nothing in the protocol arranges that for
them, so making it safe is the server's job, and the protocol says only what
the server has to guarantee, not how.

Two things have to be serialised, and they are not the same thing.

The heap allocator and whatever records which queues are handed out are shared
by every client of the server. A controller's admin queue is shared by every
client of that controller: completions are reaped in order and carry no record
of who submitted, and creating a queue submits on it too, so two commands in
flight can take each other's answers. Keeping those separate matters, because
a command can run as long as a Format does, and holding the first lock for
that long would stop clients who only wanted memory.

The rest is about not letting one client stop the others. A server reads a
message when one has arrived, with `nvme_cplane_msg_tryrecv()`, and drops a
client that stops reading its replies, with `nvme_cplane_msg_trysend()`; with
one message per packet there is no half-written message to wait out.

The reference server, in `tests/test_nvme_cplane.c`, serves one client and
reads with the blocking `nvme_cplane_msg_recv()`, so it shows the protocol
rather than the above. xNVMe's serves each connection on a thread and takes
the two locks described above.

## Liveness

The connection is the signal. A client that exits, cleanly or otherwise, has
its socket closed by the kernel, and a server reclaims the queues and the
memory it handed that client; the reference server reclaims the queue. There
is nothing to time out and no state that outlives the process it belonged to,
which is the part a shared segment could not do: a segment left behind by a
killed process still reads as plausible.

In the other direction a client watches its socket, so a server that goes away
is noticed rather than waited on forever.

## What this does not do

There is no isolation between clients. A client maps BAR0, so it can ring any
doorbell and write `CC`, which is to say it can reset the controller. All
clients of one controller share a DMA domain, so a command one client submits
can name memory another client was given. Clients are inside one trust domain
by construction. Where that is unacceptable, the answer is the kernel driver,
which arbitrates because it owns the device.

`SO_PEERCRED` tells a server who is asking, and the kernel vouches for it
since a peer cannot claim a uid it does not have. What a server does with that
is policy and stays with the server.
