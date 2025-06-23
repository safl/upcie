import pytest
from conftest import uio_devices


@pytest.mark.parametrize("bdf", uio_devices())
def test_upcie_nvme_driver(cijoe, bdf):

    err, _ = cijoe.run(f"upcie_nvme_driver {bdf}")
    assert not err
