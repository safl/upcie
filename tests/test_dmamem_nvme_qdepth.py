import pytest
from conftest import iommu_available, uio_devices


@pytest.mark.skipif(
    not iommu_available(), reason="guest exposes no IOMMU groups; vfio-pci unavailable"
)
@pytest.mark.parametrize("bdf", uio_devices())
def test_dmamem_nvme_qdepth(cijoe, bdf):
    """
    Read through an I/O qpair as deep as the request pool allows.

    Needs vfio-pci and iommufd, so it is skipped where the guest has no IOMMU.
    """

    binary = "test_dmamem_nvme_qdepth"

    err, _ = cijoe.run(f"which {binary}")
    if err:
        pytest.skip(f"{binary} is not installed on the target")

    err, _ = cijoe.run(f"devbind --device {bdf} --bind vfio-pci")
    assert not err

    err, state = cijoe.run(f"ls /sys/bus/pci/devices/{bdf}/vfio-dev/ | head -1")
    assert not err
    cdev = state.output().strip()
    assert cdev

    err, _ = cijoe.run(f"ulimit -l unlimited; {binary} /dev/vfio/devices/{cdev}")
    assert not err
