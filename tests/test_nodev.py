import pytest

SKIP = 77


def run_nodev(cijoe, binary, iommu=True):
    """
    Run a test that needs no device.

    Those that map do so into an IOAS with no device attached, which needs
    /dev/iommu and nothing else, so they skip where the target has no iommufd
    rather than fail. A binary exits 77 to skip for a reason of its own, which
    it prints.
    """

    err, _ = cijoe.run(f"which {binary}")
    if err:
        pytest.skip(f"{binary} is not installed on the target")

    err, _ = cijoe.run("test -c /dev/iommu")
    if iommu and err:
        pytest.skip("/dev/iommu is absent; the kernel has no iommufd")

    err, state = cijoe.run(binary)
    if err == SKIP:
        pytest.skip(state.output().strip().splitlines()[-1])
    assert not err


def test_dmamem_memfd_pages(cijoe):
    """A plain-page memfd, where the kernel picks and at a fixed IOVA"""

    run_nodev(cijoe, "test_dmamem_memfd_pages")


def test_dmamem_vaddr(cijoe):
    """Caller memory by address, and a range adopted by its IOVA"""

    run_nodev(cijoe, "test_dmamem_vaddr")


def test_dmamem_dmabuf_unmapped(cijoe):
    """A dma-buf held by a dmamem without an iommufd, mapping nothing"""

    run_nodev(cijoe, "test_dmamem_dmabuf_unmapped", iommu=False)


def test_vfio_map_region_at(cijoe):
    """A mapping placed over a reservation, a memfd standing in for the device"""

    run_nodev(cijoe, "test_vfio_map_region_at", iommu=False)
