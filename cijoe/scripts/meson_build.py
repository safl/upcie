#!/usr/bin/env python3
"""
Build from source
=================

Retargetable: True
------------------
"""
import errno
from argparse import ArgumentParser


def add_args(parser: ArgumentParser):
    parser.add_argument(
        "--source",
        type=str,
        default=None,
        help="path to source",
    )


def main(args, cijoe):
    """Setup and compile meson project"""

    source = args.source
    if not source:
        return errno.EINVAL

    commands = [
        "git rev-parse --short HEAD || true",
        "rm -r builddir || true",
        "meson setup builddir",
        "meson compile -C builddir",
        "cat builddir/meson-logs/meson-log.txt",
    ]
    first_err = 0
    for cmd in commands:
        err, _ = cijoe.run(cmd, cwd=source)
        if err and not first_err:
            first_err = err

    return first_err
