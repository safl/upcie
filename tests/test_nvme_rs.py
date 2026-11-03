import pytest
from conftest import iommu_available, uio_devices


@pytest.mark.parametrize("bdf", uio_devices())
def test_rust_nvme_uio(cijoe, bdf):
    """The Rust crate's NVMe example over uio_pci_generic and hugepage memory."""

    err, _ = cijoe.run("devbind --bind uio_pci_generic")
    assert not err

    err, _ = cijoe.run(f"upcie_rs_nvme {bdf}")
    assert not err


@pytest.mark.skipif(
    not iommu_available(), reason="guest exposes no IOMMU groups; vfio-pci unavailable"
)
@pytest.mark.parametrize("bdf", uio_devices())
def test_rust_nvme_vfio(cijoe, bdf):
    """The Rust crate's NVMe example over vfio-pci, iommufd and memfd memory."""

    err, _ = cijoe.run("devbind --bind vfio-pci")
    assert not err

    err, _ = cijoe.run(f"ulimit -l unlimited; upcie_rs_nvme {bdf}")
    assert not err
