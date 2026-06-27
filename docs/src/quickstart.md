# Quickstart

uPCIe is header-only, so there is nothing to build or link for the library
itself. Add the `include/` directory to your compiler's include path and
include the umbrella header:

```c
#include <upcie/upcie.h>
```

Compile your program against it:

```bash
gcc -std=c11 -Iinclude my_driver.c -o my_driver
```

Include only what you need instead of the umbrella when you want tight control
over which toolchain and UAPI headers are pulled in, for example
`#include <upcie/pci.h>` together with `#include <upcie/mmio.h>`.

The NVMe driver components are gated behind `_UPCIE_WITH_NVME`; define it before
including the umbrella header to enable them:

```c
#define _UPCIE_WITH_NVME
#include <upcie/upcie.h>
```

A complete, runnable reference driver is provided in
`example/upcie_nvme_driver.c`. It discovers a controller, maps its registers,
sets up queues, and issues commands, and it can drive the device over either
the `vfio-pci` or the `uio_pci_generic` backend.
