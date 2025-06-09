#!/usr/bin/env python3
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
# * /sys/bus/pci/devices/{slot}/driver
# * /sys/bus/pci/devices/{slot}/driver_override
# * /sys/bus/pci/devices/{slot}/driver/unbind
# * /sys/bus/pci/devices/{slot}/iommu_group
# * /sys/bus/pci/devices/{slot}/nvme/nvme*
# * /sys/bus/pci/devices/{slot}/nvme/nvme*/ng*
# * /sys/bus/pci/devices/{slot}/nvme/nvme*/nvme*
# * /sys/bus/pci/drivers/{driver_name}/bind
# * /sys/bus/pci/drivers/{driver_name}/new_id
# * /sys/bus/pci/drivers_probe
#
import sys
import os
import subprocess
import argparse
import errno
from itertools import chain
from typing import Optional
from pprint import pprint
from pathlib import Path
from dataclasses import dataclass, asdict, field

PCIE_DEFAULT_CLASSCODE = 0x0108  # Mass Storage - NVM


def run(cmd: str):
    """Run a command and capture the output"""

    print(f"cmd({cmd})")
    return subprocess.run(cmd, capture_output=True, shell=True, text=True)


def sysfs_write(path: Path, text):

    with os.fdopen(os.open(path, os.O_WRONLY), "w") as f:
        f.write(f"{text}\n")


class System(object):

    DRIVERS = {"nvme", "vfio-pci", "vfio-noiommu", "uio_pci_generic"}

    drivers: dict = {}

    def probe_drivers(self):
        pass


@dataclass
class Device:
    """Encapsulation of a PCIe device"""

    MANDATORY_KEYS = [
        "slot",
        "vendor",
        "device",
        "classcode",
    ]

    slot: str  # PCI address of the device, e.g. "0000:02:00.0"
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
        for key in Device.MANDATORY_KEYS:
            cdata[key] = data.copy().get(key)

        return cls(**cdata)

    def probe_driver(self):
        """Populate driver via sysfs; returns False if no driver is found"""

        try:
            self.driver = (
                Path(f"/sys/bus/pci/devices/{self.slot}/driver")
                .resolve(strict=True)
                .name
            )
        except FileNotFoundError:
            pass

        return self.driver != None

    def probe_iommugroup(self):
        """Populate iommugroup via sysfs; returns False if no iommugroup is found"""

        try:
            self.iommugroup = int(
                Path(f"/sys/bus/pci/devices/{self.slot}/iommu_group").resolve(
                    strict=True
                )
            )
        except FileNotFoundError:
            self.iommugroup = None

        return self.iommugroup != None

    def probe_handles(self):
        """Determine possible handles to the NVMe device"""

        for top in Path(f"/sys/bus/pci/devices/{self.slot}/nvme").glob("nvme*"):
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

    print(f"Unbinding; from('{device.driver}')")
    sysfs = Path("/sys") / "bus" / "pci"

    unbind = sysfs / "devices" / device.slot / "driver" / "unbind"
    if not unbind.exists():
        print("Not bound; skipping unbind()")
        return

    sysfs_write(unbind, device.slot)


def bind(args, device: Device, driver_name: str):
    """Bind the driver named 'driver_name' with 'device'"""

    print(f"Binding; from('{device.driver}') to ('{driver_name}')")

    unbind(args, device)

    sysfs = Path("/sys") / "bus" / "pci"

    sysfs_write(sysfs / "devices" / device.slot / "driver_override", driver_name)

    sysfs_write(
        sysfs / "drivers" / driver_name / "new_id", f"{device.vendor} {device.device}"
    )

    sysfs_write(sysfs / "drivers" / driver_name / "bind", device.slot)

    sysfs_write(sysfs / "drivers_probe", device.slot)


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
        "--props", action="store_true", help="Print properties of PCIe device."
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

    args = parser.parse_args()

    return args


def main(args):

    system = System()
    system.probe_drivers()

    devices = [
        device
        for device in device_scan(args)
        if not args.device or (args.device == device.slot)
    ]

    for cur, device in enumerate(devices, 1):
        print(f"# Device({device.slot}) -- {cur}/{len(devices)}")

        if args.props:
            print_props(args, device)

        if args.unbind:
            if device.is_used:
                print(f"Skipping unbind({device.driver}); device is in use.")
            else:
                unbind(args, device)

        if args.bind:
            if device.is_used:
                print(f"Skipping bind({args.bind}); device is in use.")
            else:
                bind(args, device, args.bind)


if __name__ == "__main__":
    ARGS = parse_args()
    try:
        sys.exit(main(ARGS))
    except PermissionError as exc:
        print(str(exc))
        print("You need to have CAP_SYS_ADMIN e.g. run as 'root' or with 'sudo'")
        sys.exit(errno.EPERM)
