#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>
#
# Tool for inspecting and configuring hugepages on Linux
#
import subprocess
import argparse
import os
import sys
import logging as log
from pathlib import Path

HUGEPAGE_MOUNTS = [
    "/dev/hugepages",
    "/mnt/huge",
    "/hugepages",
]
SYSFS_HUGEPAGES = Path("/sys/kernel/mm/hugepages")


def run(cmd: str):
    """Run a command and capture the output"""

    log.info(f"cmd({cmd})")
    return subprocess.run(cmd, capture_output=True, shell=True, text=True)


def sysfs_write(path: Path, text):
    log.info(f'{path} "{text}"')
    with os.fdopen(os.open(path, os.O_WRONLY), "w") as f:
        return f.write(f"{text}\n")


def list_supported_sizes():
    sizes = []
    for entry in SYSFS_HUGEPAGES.glob("hugepages-*kB"):
        sizes.append(entry.name.split("-")[1].replace("kB", ""))
    return sizes


def show_info(args):
    print("Hugepage Support:")
    for entry in SYSFS_HUGEPAGES.glob("hugepages-*kB"):
        size = entry.name.split("-")[1]
        nr = int((entry / "nr_hugepages").read_text())
        free = int((entry / "free_hugepages").read_text())
        resv = int((entry / "resv_hugepages").read_text())
        print(f"  Size: {size}  Total: {nr}  Free: {free}  Reserved: {resv}")


def setup_pages(args):
    """Setup hugepages via sysfs"""

    target = SYSFS_HUGEPAGES / f"hugepages-{args.size}kB" / "nr_hugepages"
    if not target.exists():
        log.error(f"Invalid hugepage size: {args.size}kB")
        sys.exit(1)

    try:
        sysfs_write(target, str(args.count))
    except PermissionError:
        log.error("Permission denied. Run as root or with sudo.")
        sys.exit(1)

    try:
        actual = int(target.read_text())
        if not actual:
            log.error(
                f"No hugepages were reserved out of count({args.count}) for size({args.size}) kB"
            )
        elif actual < args.count:
            log.warning(
                f"Only {actual} hugepage(s) were reserved out of count({args.count}) for size({args.size}) kB"
            )
    except Exception as exc:
        log.error(f"Failed to verify hugepage allocation: {exc}")
        sys.exit(1)


def mount_hugetlbfs(args):
    mountpoint = Path(args.mountpoint or "/dev/hugepages")
    if not mountpoint.exists():
        mountpoint.mkdir(parents=True)

    cmd = f"mount -t hugetlbfs nodev {mountpoint}"
    if args.pagesize:
        cmd += f" -o pagesize={args.pagesize}k"
    result = run(cmd)
    if result.returncode != 0:
        log.error(f"Failed to mount hugetlbfs: {result.stderr}")
        sys.exit(1)
    print(f"Mounted hugetlbfs at {mountpoint}")


def parse_args():

    try:
        supported_sizes = list_supported_sizes()
    except Exception as exc:
        supported_sizes = []
        log.warning(f"Could not read supported hugepage sizes: {exc}")

    parser = argparse.ArgumentParser(description="Manage Linux Hugepages")

    parser.add_argument("--verbose", action="store_true", help="Enable verbose logging")

    subparsers = parser.add_subparsers(dest="command")

    subparsers.add_parser("info", help="Show hugepage status and capabilities")

    setup = subparsers.add_parser("setup", help="Configure hugepage pool")

    setup.add_argument(
        "--size",
        choices=supported_sizes if supported_sizes else None,
        default=supported_sizes[0] if supported_sizes else None,
        help="Hugepage size in kB",
    )

    setup.add_argument(
        "--count", required=True, type=int, help="Number of pages to reserve"
    )

    mount = subparsers.add_parser("mount", help="Mount hugetlbfs")
    mount.add_argument("--mountpoint", help="Mount location (default: /dev/hugepages)")
    mount.add_argument("--pagesize", help="Optional hugepage size in kB")

    return parser.parse_args()


def main():
    args = parse_args()

    log.basicConfig(
        level=log.DEBUG if args.verbose else log.INFO,
        format="# %(levelname)s: %(message)s",
    )

    if args.command == "info":
        show_info(args)
    elif args.command == "setup":
        setup_pages(args)
    elif args.command == "mount":
        mount_hugetlbfs(args)
    else:
        log.error("No command specified. Use --help.")
        sys.exit(1)


if __name__ == "__main__":
    main()
