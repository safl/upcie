import pytest
from conftest import iommu_available


def test_devbind_list(cijoe):
    err, _ = cijoe.run("devbind --list")
    assert not err


def test_devbind_unbind(cijoe):
    err, _ = cijoe.run("devbind --unbind")
    assert not err


def test_devbind_bind_nvme(cijoe):
    err, _ = cijoe.run("devbind --bind nvme")
    assert not err


def test_devbind_bind_uio(cijoe):
    err, _ = cijoe.run("devbind --bind uio_pci_generic")
    assert not err


@pytest.mark.skipif(
    not iommu_available(), reason="guest exposes no IOMMU groups; vfio-pci unavailable"
)
def test_devbind_bind_vfio(cijoe):
    err, _ = cijoe.run("devbind --bind vfio-pci")
    assert not err
