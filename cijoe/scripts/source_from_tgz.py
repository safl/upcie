"""
Prepare source directory using a .tar.gz archive
=================================================

Transfers the archive from step.with.artifacts to step.with.source and
extracts its content.

Retargetable: True
------------------
"""

import logging as log
from argparse import ArgumentParser


def add_args(parser: ArgumentParser):
    parser.add_argument(
        "--artifacts",
        type=str,
        default="/tmp/artifacts",
        help="path to where the artifacts are located locally",
    )
    parser.add_argument(
        "--source",
        type=str,
        default="/tmp/upcie_source",
        help="path to where the source should be transferred to",
    )


def main(args, cijoe):
    """Transfer and extract source archive"""

    artifacts = args.artifacts
    source = args.source

    cijoe.run(
        f'[ -d "{source}" ] && mv "{source}" "{source}-$(date +%Y%m%d%H%M%S)" || true'
    )

    err, _ = cijoe.run(f"mkdir -p {source}")
    if err:
        return err

    filename = "upcie-src.tar.gz"
    cijoe.put(
        f"{artifacts}/{filename}",
        f"{source}/{filename}",
    )

    commands = [
        f"ls -lh {source}",
        "tar xzf upcie-src.tar.gz --strip 1",
        "rm upcie-src.tar.gz",
        "df -h",
        "ls -lh",
    ]

    first_err = 0
    for command in commands:
        err, _ = cijoe.run(command, cwd=f"{source}")
        if err:
            log.error(f"cmd({command}), err({err})")
            first_err = first_err if first_err else err

    return first_err
