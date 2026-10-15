import pytest


def test_nvme_admin_timeout(cijoe):
    """
    Run the check that an admin command may take longer than CAP.TO.

    The queues and registers are host memory, so unlike the rest of the suite this
    needs no device and runs wherever uPCIe is deployed.
    """

    binary = "test_nvme_admin_timeout"

    err, _ = cijoe.run(f"which {binary}")
    if err:
        pytest.skip(f"{binary} is not installed on the target")

    err, _ = cijoe.run(binary)
    assert not err
