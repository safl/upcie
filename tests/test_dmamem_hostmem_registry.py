import pytest


def test_dmamem_hostmem_registry(cijoe):
    """
    Translate a hugepage region by physical address, and register memory beside it.

    Needs hugepages and CAP_SYS_ADMIN, for pagemap, but no device and no IOMMU.
    """

    binary = "test_dmamem_hostmem_registry"

    err, _ = cijoe.run(f"which {binary}")
    if err:
        pytest.skip(f"{binary} is not installed on the target")

    err, _ = cijoe.run(binary)
    assert not err
