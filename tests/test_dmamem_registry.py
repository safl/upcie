import pytest


def test_dmamem_registry(cijoe):
    """
    Run the registry bookkeeping checks.

    Adoption takes its addresses from a caller-supplied table, so this needs no
    device and runs wherever uPCIe is deployed.
    """

    binary = "test_dmamem_registry"

    err, _ = cijoe.run(f"which {binary}")
    if err:
        pytest.skip(f"{binary} is not installed on the target")

    err, _ = cijoe.run(binary)
    assert not err
