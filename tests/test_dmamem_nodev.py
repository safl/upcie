import pytest

SKIP = 77


def run_nodev(cijoe, binary):
    """
    Run a dmamem test that maps into an IOAS with no device attached.

    These need /dev/iommu and nothing else, so they skip where the target has
    no iommufd rather than fail. A binary exits 77 to skip for a reason of its
    own, which it prints.
    """

    err, _ = cijoe.run(f"which {binary}")
    if err:
        pytest.skip(f"{binary} is not installed on the target")

    err, _ = cijoe.run("test -c /dev/iommu")
    if err:
        pytest.skip("/dev/iommu is absent; the kernel has no iommufd")

    err, state = cijoe.run(binary)
    if err == SKIP:
        pytest.skip(state.output().strip().splitlines()[-1])
    assert not err


def test_dmamem_memfd_pages(cijoe):
    """A plain-page memfd, where the kernel picks and at a fixed IOVA"""

    run_nodev(cijoe, "test_dmamem_memfd_pages")
