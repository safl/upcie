import pytest

def test_upcie_nvme_driver(cijoe):
    for bdf, val in cijoe.getconf("devices.pcie", {}).items():
        err, _ = cijoe.run(f"upcie_nvme_driver {bdf}")
        assert not err
