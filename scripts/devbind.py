#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>
#
# Get info about and control driver associcated with NVMe devices
#
# This makes use of the following tools:
#
# * lspci -Dvmmnk
# * lsof {devhandle1, devhandle2, ... devhandleN}
#
# The following sysfs entries for driver bindings:
#
# * /sys/bus/pci/devices/{bdf}/driver
# * /sys/bus/pci/devices/{bdf}/driver_override
# * /sys/bus/pci/devices/{bdf}/driver/unbind
# * /sys/bus/pci/devices/{bdf}/iommu_group
# * /sys/bus/pci/devices/{bdf}/nvme/nvme*
# * /sys/bus/pci/devices/{bdf}/nvme/nvme*/ng*
# * /sys/bus/pci/devices/{bdf}/nvme/nvme*/nvme*
# * /sys/bus/pci/drivers/{driver_name}/bind
#
# The following could, but currently are not, be used for automatic detection based on
# class-code etc.
#
# * /sys/bus/pci/drivers/{driver_name}/new_id
# * /sys/bus/pci/drivers_probe
#
import sys
import os
import subprocess
import argparse
import errno
import time
import logging as log
from itertools import chain
from typing import Optional
from pprint import pprint
from pathlib import Path
from dataclasses import dataclass, asdict, field

PCIE_DEFAULT_CLASSCODE = 0x0108  # Mass Storage - NVM


def run(cmd: str):
    """Run a command and capture the output"""
    log.info(f"cmd({cmd})")
    return subprocess.run(cmd, capture_output=True, shell=True, text=True)


def sysfs_write(path: Path, text):
    log.info(f'{path} "{text}"')
    with os.fdopen(os.open(path, os.O_WRONLY), "w") as f:
        f.write(f"{text}\n")


class System(object):

    DRIVERS = {"nvme", "vfio-pci", "vfio-noiommu", "uio_pci_generic"}

    drivers: dict = {}

    def probe_drivers(self):

        loaded = set(
            (
                path.name
                for path in Path(f"/sys/bus/pci/drivers").resolve(strict=True).glob("*")
            )
        )

        missing = self.DRIVERS - loaded

        for driver_name in self.DRIVERS:
            self.drivers[driver_name] = {
                "available": driver_name not in missing,
            }

    def pp(self):
        print("system:")
        print("  drivers:")
        for driver_name, props in self.drivers.items():
            print(f"  - {driver_name}: {props}")


@dataclass
class Device:
    """Encapsulation of a PCIe device"""

    MANDATORY_KEYS = {
        "slot": "bdf",
        "vendor": "vendor",
        "device": "device",
        "classcode": "classcode",
    }

    bdf: str  # PCI address of the device, e.g. "0000:02:00.0"
    vendor: str  # Vendor ID (hex), e.g. "144d" for Samsung
    device: str  # Device ID (hex), identifies the specific device model
    classcode: str  # PCI class code (hex), e.g. "0108" for NVMe controller

    driver: Optional[str] = None  # Name of the driver bound to the device, e.g. "nvme"
    iommugroup: Optional[int] = None  # IOMMU group number the device belongs to

    is_used: bool = True  # Whether or not the device is in use; assume it is
    handles: list = field(default_factory=list)

    @classmethod
    def from_dict(cls, data: dict) -> "Device":
        cdata = {}
        for src, tgt in Device.MANDATORY_KEYS.items():
            cdata[tgt] = data.copy().get(src)

        return cls(**cdata)

    def probe_driver(self):
        """Populate driver via sysfs; returns False if no driver is found"""

        try:
            self.driver = (
                Path(f"/sys/bus/pci/devices/{self.bdf}/driver")
                .resolve(strict=True)
                .name
            )
        except FileNotFoundError:
            pass
        return self.driver is not None

    def probe_iommugroup(self):
        """Populate iommugroup via sysfs; returns False if no iommugroup is found"""

        try:
            self.iommugroup = int(
                Path(f"/sys/bus/pci/devices/{self.bdf}/iommu_group")
                .resolve(strict=True)
                .name
            )
        except FileNotFoundError:
            self.iommugroup = None
        return self.iommugroup is not None

    def probe_handles(self):
        """Determine possible handles to the NVMe device"""

        for top in Path(f"/sys/bus/pci/devices/{self.bdf}/nvme").glob("nvme*"):
            for bottom in chain(top.glob("ng*"), top.glob("nvme*")):
                for path in Path("/dev").glob(f"{bottom.name}*"):
                    self.handles.append(str(path))

    def probe_usage(self):
        """Attempt to determine whether the device is in use"""

        if not self.handles:
            self.is_used = False
            return

        handles = " ".join(self.handles)
        proc = run(f"lsof {handles}")

        self.is_used = bool(proc.stdout)


def device_scan(args):
    """Yields a Device for each PCIe device with classcode(args.classcode)"""

    proc = run("lspci -Dvmmnk")

    props = {}
    for line in proc.stdout.splitlines():
        if not line:
            if int(props.get("classcode", "0"), 16) == args.classcode:
                device = Device.from_dict(props)
                device.probe_handles()
                device.probe_usage()
                device.probe_driver()
                device.probe_iommugroup()
                yield device

            props = {}
            continue

        key, val = [txt.strip().lower() for txt in str(line).split(":", 1)]
        if key == "class":
            key = "classcode"

        props[key] = val


def print_props(args, device: Device):
    """Pretty-print the properties of a device"""

    print(f"props:")
    for key, val in asdict(device).items():
        if isinstance(val, int) or isinstance(val, list):
            print(f"  {key}: {val}")
        else:
            print(f"  {key}: '{val}'")


def unbind(args, device: Device):
    log.info(f"Unbinding({device.bdf}) from '{device.driver}'")

    driver_path = Path("/sys") / "bus" / "pci" / "devices" / device.bdf / "driver"

    unbind = driver_path / "unbind"
    if not unbind.exists():
        log.info("Not bound; skipping unbind()")
        return

    sysfs_write(unbind, device.bdf)


def bind(args, device: Device, driver_name: str):
    """Bind the driver named 'driver_name' with 'device'"""

    unbind(args, device)

    log.info(f"Binding({device.bdf}) to '{driver_name}'")

    sysfs = Path("/sys") / "bus" / "pci"

    sysfs_write(sysfs / "devices" / device.bdf / "driver_override", driver_name)

    max_attempts = 10
    for attempt in range(1, max_attempts + 1):
        try:
            sysfs_write(sysfs / "drivers" / driver_name / "bind", device.bdf)
            break
        except OSError as exc:
            if attempt == max_attempts or exc.errno != errno.EBUSY:
                log.error(f"Could not bind despite {max_attempts} retries.")
                raise
            delay = attempt * 1
            log.info(f"Retrying in in {delay} second(s)")
            time.sleep(delay)

    # Enable BUS-mastering (tell it that it can initiate DMA)
    if driver_name == "uio_pci_generic":
        log.info(f"Running setpci to enable bus-mastering; driver_name({driver_name})")
        proc = run(f"setpci -s {device.bdf} COMMAND=0x06")
    else:
        log.info(f"Not running setpci; driver_name({driver_name})")


def parse_args():
    parser = argparse.ArgumentParser(description="Parse device binding options.")

    parser.add_argument(
        "--classcode",
        default=PCIE_DEFAULT_CLASSCODE,
        help="The class of PCIe devices to scan for",
    )

    parser.add_argument(
        "--device",
        required=False,
        help="Instead of all; then only the given PCI address.",
    )

    parser.add_argument(
        "--list",
        action="store_true",
        help="Print PCIe device(s); such as their 'bdf' and driver-association.",
    )

    parser.add_argument("--unbind", action="store_true", help="Unbind if bound.")

    def parse_bind(value):
        if value in System.DRIVERS:
            return value
        return Path(value)

    parser.add_argument(
        "--bind",
        type=parse_bind,
        help="Unbind if bound; then bind to the given driver-name [nvme, vfio-pci, uio-pci-generic] or to a .ko driver file (path)",
    )

    parser.add_argument(
        "--verbose", action="store_true", help="Print log-messages beyond errors"
    )

    args = parser.parse_args()

    return args


def main(args):

    system = System()
    system.probe_drivers()

    if args.list:
        system.pp()

    devices = [
        device
        for device in device_scan(args)
        if not args.device or (args.device == device.bdf)
    ]

    for cur, device in enumerate(devices, 1):
        log.info(f"Device({device.bdf}) -- {cur}/{len(devices)}")

        if args.list:
            print_props(args, device)

        if args.unbind:
            if device.is_used:
                log.info(f"Skipping unbind({device.driver}); device is in use.")
            else:
                unbind(args, device)

        if args.bind:
            if device.is_used:
                log.info(f"Skipping bind({args.bind}); device is in use.")
            else:
                bind(args, device, args.bind)


if __name__ == "__main__":
    ARGS = parse_args()

    log.basicConfig(
        level=log.DEBUG if ARGS.verbose else log.INFO,
        format="# %(levelname)s: %(message)s",
    )

    try:
        sys.exit(main(ARGS))
    except PermissionError as exc:
        log.error(str(exc))
        log.error("You need to have CAP_SYS_ADMIN e.g. run as 'root' or with 'sudo'")
        sys.exit(errno.EPERM)
