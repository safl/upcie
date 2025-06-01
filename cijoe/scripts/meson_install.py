#!/usr/bin/env python3
"""
Install meson-project from previously built source
==================================================


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
    source = args.source
    if not source:
        return errno.EINVAL

    commands = [
        "meson install",
        "meson --internal uninstall",
        "meson install",
        "cat meson-logs/meson-log.txt",
    ]
    for cmd in commands:
        err, _ = cijoe.run(cmd, cwd=f"{source}/builddir")
        if err:
            return err

    return 0
