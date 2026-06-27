# Building and testing

The headers and the bundled C tests build with Meson via the top-level
Makefile:

```bash
make config        # meson setup builddir
make build         # meson compile -C builddir
```

The compiled C tests land in `builddir/tests` and expect a suitable environment,
such as reserved hugepages or a device bound to a user-space driver.

## Guest-based testing

The integration tests build, install, and run uPCIe on a *target* machine,
following the same cijoe-driven model as xNVMe. The target is described entirely
by a cijoe configuration: how to reach it over SSH (the `cijoe.transport`
section), and, for a QEMU guest, where its disk image lives
(`system_imaging.toml`) and how to boot it (the `qemu` and `guest_image`
sections of the guest config).

The default guest image is a [nosi](https://github.com/safl/nosi) build, which
already ships the `devbind`, `hugepages`, and `iommu` tools the test-suite uses.

The default target is a QEMU guest. Provisioning boots it, then builds and
installs uPCIe inside it; the test-suite then runs against it. `make verify`
covers both IOMMU modes back to back:

```bash
make provision               # boot the guest, build and install uPCIe inside it
make test                    # run the test-suite against the target
make verify                  # provision and test across both IOMMU modes
make verify-iommu-disabled   # just the IOMMU-off path (uio_pci_generic)
make verify-iommu-enabled    # just the IOMMU-on path (vfio-pci)
```

The two modes differ in the QEMU machine, so each is a full provision-and-test
cycle rather than two passes against one guest. CI runs both on every push and
pull request. `CIJOE_CONFIG` selects the config for the bare provision, test,
and guest targets if you need to override it.

Provisioning pulls a multi-gigabyte guest image and requires `/dev/kvm`, QEMU,
and cijoe on the host. The image is resolved and pulled with
[withcache](https://github.com/safl/withcache).

## Retargeting

The build, install, and test steps run over the configured transport, so the
same flow re-targets to another machine by changing only the transport. Point
`cijoe.transport` at the local host, or at a remote machine over SSH, to build
and test there instead of in the QEMU guest. The QEMU provisioning steps are
specific to the guest target; the build, install, and test steps are not.
