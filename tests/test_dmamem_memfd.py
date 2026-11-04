import pytest
from conftest import iommu_available


@pytest.mark.skipif(
    not iommu_available(), reason="guest exposes no IOMMU groups; /dev/iommu unusable"
)
def test_dmamem_memfd(cijoe):
    """
    Map a memfd heap into an IOAS, at the IOVA the kernel picks and at its address.

    Needs /dev/iommu and hugepages but no device.
    """

    binary = "test_dmamem_memfd"

    err, _ = cijoe.run(f"which {binary}")
    if err:
        pytest.skip(f"{binary} is not installed on the target")

    err, _ = cijoe.run(binary)
    assert not err
