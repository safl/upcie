import pytest


def test_nvme_qid(cijoe):
    """
    Run the qid bitmap accounting checks.

    Pure bitmap logic, so unlike the rest of the suite this needs no device and
    runs wherever uPCIe is deployed.
    """

    binary = "test_nvme_qid"

    err, _ = cijoe.run(f"which {binary}")
    if err:
        pytest.skip(f"{binary} is not installed on the target")

    err, _ = cijoe.run(binary)
    assert not err
