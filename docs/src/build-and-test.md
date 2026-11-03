# Building and testing

The headers and the bundled C tests build with Meson via the top-level
Makefile:

```bash
make config        # meson setup builddir
make build         # meson compile -C builddir
```

The compiled C tests land in `builddir/tests` and expect a suitable environment,
such as reserved hugepages or a device bound to a user-space driver.

## From a Meson project

`dependency('upcie')` finds an installed uPCIe through its `upcie.pc`, or
falls back to a wrap in the project's `subprojects/upcie.wrap`:

```ini
[wrap-git]
url = https://github.com/safl/upcie.git
revision = <a commit or tag>
depth = 1

[provide]
dependency_names = upcie
```

As a subproject uPCIe declares its headers and builds neither the example nor
the tests. It installs its headers and `upcie.pc` into the project's prefix,
as Meson subprojects do; a project that builds against uPCIe without shipping
it sets `-Dupcie:skip-install=true`, or `default_options:
['skip-install=true']` on its `subproject('upcie')` call.
`tests/meson/check.sh` builds a consumer both ways, and CI runs it.

## Guest-based testing

The integration tests build, install, and run uPCIe on a *target* machine,
following the same cijoe-driven model as xNVMe. The target is described entirely
by a cijoe configuration: how to reach it over SSH (the `cijoe.transport`
section), and, for a QEMU guest, which image it boots (`system_imaging.toml` and
the release config's `oras.url`), how (the `qemu` section of the guest config),
and with which devices (a rig in `cijoe/rigs/`).

The guest is booted by the rig scripts of
[xnvme-devenv](https://github.com/xnvme/xnvme-devenv), whose host container
carries QEMU, cijoe and those scripts; CI runs the guest jobs in it. A rig is a
cijoe config giving the guest's PCIe topology and devices. uPCIe's own,
`upcie-nvme`, has the one NVMe controller the tests drive, at `0000:01:00.0`;
xnvme-devenv's, under `/opt/xnvme-devenv/rigs` in the container, add PCIe
switches, an emulated GPU and NIC, and more controllers. The default guest image
is a [nosi](https://github.com/safl/nosi) build, which already ships the
`devbind`, `hugepages`, and `iommu` tools the test-suite uses.

`make guest` brings the guest up, `make deploy` builds and installs uPCIe on it,
and `make test` runs the suite. `make verify` chains all three across both IOMMU
modes. `make guest` needs the rig scripts, so it runs in the host container;
`make devenv-verify` starts the container and runs `make verify` in it:

```bash
make devenv-verify           # verify, in xnvme-devenv's host container
make devenv-run CMD="..."    # any command in the container, e.g. make guest
make devenv-down             # remove the container; its guests stay in .devenv

make guest                   # in the container: bring up the QEMU guest
make deploy                  # build and install uPCIe on the target
make test                    # run the test-suite against the target
make verify                  # guest, deploy, and test across both IOMMU modes
make verify-iommu-disabled   # just the IOMMU-off path (uio_pci_generic)
make verify-iommu-enabled    # just the IOMMU-on path (vfio-pci)
```

The two modes differ in the QEMU machine, so each is a full cycle rather than two
passes against one guest. CI runs both on every push and pull request.
`CIJOE_CONFIG` selects the config for the bare guest, deploy, and test targets,
`RIG` the rig, and `RELEASE` the Ubuntu release the guest runs.

Provisioning pulls a multi-gigabyte guest image and requires `/dev/kvm` and
docker or podman (`DEVENV_ENGINE=podman`) on the host.

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
