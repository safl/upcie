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

The default target is a QEMU guest: `make guest` brings it up, `make deploy`
builds and installs uPCIe on it, and `make test` runs the suite. `make verify`
chains all three across both IOMMU modes:

```bash
make guest                   # bring up the QEMU guest
make deploy                  # build and install uPCIe on the target
make test                    # run the test-suite against the target
make verify                  # guest, deploy, and test across both IOMMU modes
make verify-iommu-disabled   # just the IOMMU-off path (uio_pci_generic)
make verify-iommu-enabled    # just the IOMMU-on path (vfio-pci)
```

The two modes differ in the QEMU machine, so each is a full cycle rather than two
passes against one guest. CI runs both on every push and pull request.
`CIJOE_CONFIG` selects the config for the bare guest, deploy, and test targets.

Provisioning pulls a multi-gigabyte guest image and requires `/dev/kvm`, QEMU,
and cijoe on the host. The image is resolved and pulled with
[withcache](https://github.com/safl/withcache).

## Retargeting

The bring-up is deliberately a separate task from the deploy. `guest_setup.yaml`
is QEMU-specific (it depends on the qemu and system-imaging config), while
`deploy.yaml` and `test.yaml` run over the configured transport and do not care
what is on the other end. So they retarget to a real machine unchanged: point a
config's first transport at it (see `configs/hw-example.toml`) and run

```bash
make gen-artifacts
make deploy test CIJOE_CONFIG=configs/hw-example.toml
```

skipping `make guest` entirely. The deploy builds and installs uPCIe on the
target and loads the drivers and hugepages, then the test-suite runs there.
