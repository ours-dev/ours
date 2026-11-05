#!/usr/bin/env python3
"""Adds a block device to a running QEMU machine through its QMP socket --
the host side of the PCI hot-plug demo (the PCI hot-plug round, step 4).

The guest's resident drivers see nothing on their own: firmware assigns
BARs at boot only, so a device that arrives later carries no base address,
and it is the guest's rescan (pci-rescan) that sizes, assigns, enables and
publishes it. This script only makes QEMU present the function on the bus.

Start QEMU with QMP=<path> (scripts/qemu_boot.py), then:

    python3 scripts/qemu_device.py --qmp <path> add <image> [--kind nvme|virtio]
    python3 scripts/qemu_device.py --qmp <path> list
    python3 scripts/qemu_device.py --qmp <path> del <id>

`add` attaches the raw file |image| behind a drive backend named after the
device's id (ours-hotN, N the lowest free number) and a device of --kind:
nvme (QEMU's NVMe controller, the disk's serial ours-hot) or virtio
(virtio-blk-pci). The id is printed; `del` takes it back -- which needs the
guest's ACPI eject to actually remove the function, and the guest has no
AML interpreter, so a del reports what QEMU did and the round documents the
removal direction as not done. `list` says what QEMU holds.

The machine's own disks carry ids d0 (virtio) and nvme0 (NVMe) from
scripts/qemu_boot.py; the demo's never collide with those.
"""

from __future__ import annotations

import argparse
import json
import socket
import sys
from pathlib import Path


class Qmp:
    """One QMP session over a Unix socket: the greeting, the negotiate, and
    one command at a time, each answered by exactly one response."""

    def __init__(self, path: str) -> None:
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(path)
        self.file = self.sock.makefile("rw", encoding="utf-8")
        # The greeting, then the negotiate's empty reply.
        self._read()
        self.command("qmp_capabilities")

    def _read(self) -> dict:
        while True:
            line = self.file.readline()
            if not line:
                raise SystemExit("qmp: the socket closed")
            message = json.loads(line)
            # Events ride the same stream; the response to a command never
            # carries one.
            if "event" in message:
                continue
            return message

    def command(self, name: str, **arguments) -> dict:
        self.file.write(json.dumps({"execute": name, "arguments": arguments}) + "\n")
        self.file.flush()
        response = self._read()
        if "error" in response:
            raise SystemExit(f"qmp: {name} answered {response['error']['desc']}")
        return response.get("return", {})

    def close(self) -> None:
        self.file.close()
        self.sock.close()


def lowest_free(held: set[str]) -> int:
    number = 0
    while f"ours-hot{number}" in held:
        number += 1
    return number


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--qmp", required=True, help="the QMP socket's path")
    sub = parser.add_subparsers(dest="action", required=True)

    p_add = sub.add_parser("add", help="attach a block device")
    p_add.add_argument("image", help="the raw file to back the device with")
    p_add.add_argument("--kind", choices=["nvme", "virtio"], default="nvme")
    p_add.add_argument("--bus", default=None,
                       help="the bus to plug into, e.g. a pcie-root-port's id; default: "
                            "QEMU's own choice, which a q35 machine refuses (its root "
                            "bus takes no hotplug)")

    sub.add_parser("list", help="what QEMU holds")

    p_del = sub.add_parser("del", help="take a device back (see the round's note)")
    p_del.add_argument("id")

    args = parser.parse_args(argv)
    qmp = Qmp(args.qmp)
    try:
        if args.action == "list":
            drives = qmp.command("query-block")
            for drive in drives:
                device = drive.get("device", "?")
                inserted = drive.get("inserted") or {}
                print(f"{device}: {inserted.get('file', 'no medium')}")
            return 0

        if args.action == "del":
            # The del — device_del — asks the guest's ACPI to eject the
            # function. A guest with no AML interpreter never answers, and
            # QEMU holds the device until it does; the round documents the
            # removal direction as not done, and this reports that state
            # rather than pretending the device left.
            qmp.command("device_del", id=args.id)
            print(f"ours: {args.id}: QEMU has asked the guest's ACPI to eject it; "
                  "this guest has no AML interpreter, so the device stays "
                  "until the machine ends. The removal direction is not "
                  "done this round (see the round's records).")
            return 0

        # add: the drive backend, then the device of the kind asked. The
        # virtio device keeps its legacy I/O BAR: QEMU drops it on a PCIe
        # link by default (disable-legacy on), and the guest's driver runs
        # the legacy interface.
        image = Path(args.image).resolve()
        if not image.is_file():
            raise SystemExit(f"qmp: {image} is not a file")
        held = {drive.get("device", "") for drive in qmp.command("query-block")}
        identifier = f"ours-hot{lowest_free(held)}"
        bus = {"bus": args.bus} if args.bus else {}
        qmp.command("blockdev-add", node_name=f"node-{identifier}", driver="raw",
                    file={"driver": "file", "filename": str(image)},
                    read_only=False)
        if args.kind == "nvme":
            qmp.command("device_add", driver="nvme", id=identifier, drive=f"node-{identifier}",
                        serial="ours-hot", **bus)
        else:
            qmp.command("device_add", driver="virtio-blk-pci", id=identifier,
                        drive=f"node-{identifier}", **bus, **{"disable-legacy": "off"})
        print(identifier)
        return 0
    finally:
        qmp.close()


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
