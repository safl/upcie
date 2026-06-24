import pytest
from conftest import iommu_available, uio_devices


@pytest.mark.parametrize("bdf", uio_devices())
def test_upcie_nvme_driver_uio(cijoe, bdf):
    """Drive the controller through the uio_pci_generic (sysfs) backend."""

    err, _ = cijoe.run("devbind --bind uio_pci_generic")
    assert not err

    err, _ = cijoe.run(f"upcie_nvme_driver {bdf}")
    assert not err


@pytest.mark.skipif(
    not iommu_available(), reason="guest exposes no IOMMU groups; vfio-pci unavailable"
)
@pytest.mark.parametrize("bdf", uio_devices())
def test_upcie_nvme_driver_vfio(cijoe, bdf):
    """Drive the controller through the vfio-pci backend (requires an IOMMU)."""

    err, _ = cijoe.run("devbind --bind vfio-pci")
    assert not err

    err, _ = cijoe.run(f"upcie_nvme_driver {bdf}")
    assert not err
