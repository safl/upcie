import pytest


def test_dmamem_shared_hostmem(cijoe):
    """
    Hand a heap description to a second mapping and check it still resolves.

    Needs hugepages but no device, so it runs whether or not the guest has an
    IOMMU.
    """

    binary = "test_dmamem_shared_hostmem"

    err, _ = cijoe.run(f"which {binary}")
    if err:
        pytest.skip(f"{binary} is not installed on the target")

    err, _ = cijoe.run(binary)
    assert not err
