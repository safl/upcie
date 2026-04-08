import pytest
from conftest import uio_devices

CASES = [
    (1, 1, 1),
    (1, 128, 128),
    (4, 128, 512),
    (4, 128, 1024),
    (4, 128, 600),
    (4, 128, 100),
]


@pytest.mark.parametrize("bdf", uio_devices())
def test_cuda_nvme_readwrite(cijoe, bdf):
    binary = "test_cuda_nvme_readwrite"

    err, _ = cijoe.run(f"which {binary}")
    if err:
        pytest.skip("test_cuda_nvme_readwrite not found; CUDA not supported")

    for num_queues, queue_depth, num_ios in CASES:
        err, _ = cijoe.run(f"{binary} {bdf} {num_queues} {queue_depth} {num_ios}")
        assert not err
