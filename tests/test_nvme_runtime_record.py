import pytest
from conftest import iommu_available, uio_devices


@pytest.mark.parametrize("bdf", uio_devices())
def test_runtime_record_uio(cijoe, bdf):
    """Publish a record and rebuild a controller from it, reached through sysfs."""

    err, _ = cijoe.run("devbind --bind uio_pci_generic")
    assert not err

    err, _ = cijoe.run(f"test_nvme_runtime_record {bdf}")
    assert not err


@pytest.mark.skipif(
    not iommu_available(), reason="guest exposes no IOMMU groups; vfio-pci unavailable"
)
@pytest.mark.parametrize("bdf", uio_devices())
def test_runtime_record_vfio(cijoe, bdf):
    """The same with the device reached through vfio-pci.

    What differs is underneath the record: the heap is mapped into an IOMMU
    domain rather than translated with addresses read from pagemap, so this is
    the arrangement a client is unprivileged under.
    """

    err, _ = cijoe.run("devbind --bind vfio-pci")
    assert not err

    err, _ = cijoe.run(f"test_nvme_runtime_record {bdf} vfio")
    assert not err
