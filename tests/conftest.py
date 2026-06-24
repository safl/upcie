from functools import lru_cache
import yaml
import pytest


@lru_cache(maxsize=None)
def uio_devices():
    """Return a list of BDFs of PCIe devices bound to 'uio_pci_generic'."""

    cijoe = pytest.cijoe_instance
    devices = []

    err, state = cijoe.run("devbind --list")
    for data in state.output().split("props:")[1:]:
        data = yaml.safe_load("props:\n" + data)
        props = data.get("props")

        if props.get("is_used"):
            continue

        devices.append(props.get("bdf"))

    return devices


@lru_cache(maxsize=None)
def iommu_available():
    """Return True when the guest exposes IOMMU groups, i.e. vfio-pci is usable."""

    cijoe = pytest.cijoe_instance

    err, state = cijoe.run("ls /sys/kernel/iommu_groups")
    if err:
        return False

    return bool(state.output().strip())
