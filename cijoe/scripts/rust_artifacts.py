"""
Install the Rust example binaries on the target
===============================================

Copies the NVMe example of the Rust crate, built by 'make rust-artifacts', from
step.with.artifacts to /usr/local/bin/upcie_rs_nvme on the target, and fails
when it is not there, so the Rust tests never run against a missing binary.

Retargetable: True
------------------
"""

import logging as log
import os
from argparse import ArgumentParser


def add_args(parser: ArgumentParser):
    parser.add_argument(
        "--artifacts",
        type=str,
        default="/tmp/artifacts",
        help="path to where the artifacts are located locally",
    )


def main(args, cijoe):
    """Copy the Rust NVMe example to the target, when it was built"""

    binary = os.path.join(args.artifacts, "upcie-rs-nvme")
    if not os.path.exists(binary):
        log.error(f"no {binary}; build it with 'make rust-artifacts'")
        return 1

    cijoe.put(binary, "/usr/local/bin/upcie_rs_nvme")
    err, _ = cijoe.run("chmod +x /usr/local/bin/upcie_rs_nvme")
    return err
