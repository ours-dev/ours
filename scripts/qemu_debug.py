#!/usr/bin/env python3
"""Boot an ours kernel image halted under QEMU with the gdbstub open, and
publish the matching kernel ELF at a fixed path for the debugger.

This is the half of one-click debugging that VS Code cannot express in a task:
it has to clear away a previous session, start QEMU, wait until the stub is
actually accepting connections, and only then announce itself -- because the
debugger attaches the moment the task reports ready, and attaching to a port
nothing is listening on fails the whole launch.

Usable on its own too:

    scripts/qemu_debug.py                       # bringup x86 dbg.1
    scripts/qemu_debug.py bringup x86 dbg.0
    PORT=1235 scripts/qemu_debug.py
    IMAGE=/path/to.obi ELF=/path/to/elf scripts/qemu_debug.py   # off-manifest

then in another terminal:

    lldb -o 'command script import scripts/lldb_ours.py' \\
         -o 'target create out/.debug/kernel.elf' \\
         -o 'target modules load --file kernel.elf --slide 0xffffff8840000000' \\
         -o 'gdb-remote localhost:1234'

The import is not optional: cl/cpus/nodes/kthread come from that script, F5 gets
it from the launch configuration's initCommands, and a bare lldb does not look
for it. This script prints the whole line when the stub comes up.

Three levels, narrowing: the product says which images exist, the product and
the architecture together say which build directory (out/<product>.<arch>, the
same one tools/set.py writes), and the switchset picks one image out of that
product's set. None of the three is a list kept in this file -- pass a wrong
one and it prints what the build actually has.
"""

from __future__ import annotations

import argparse
import atexit
import glob
import json
import os
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SWITCH_SET = "//build/switch/set/"

# Where physboot loads the kernel: KERNEL_ASPACE_BASE + ARCH_PHYSMAP_SIZE +
# 1 GiB. Printed rather than left for the reader to look up; it moves if KASLR
# is ever switched on, and the boot log's "Reserve area k:head" line is the
# authority then.
SLIDE = "0xffffff8840000000"


def die(*lines: str) -> "typing.NoReturn":  # noqa: F821
    sys.exit("\n".join("qemu-debug: " + line for line in lines))


def note(message: str) -> None:
    print(f"qemu-debug: {message}", flush=True)


# --------------------------------------------------------------------------
# Resolving the three answers into an image and the ELF whose symbols match it.
#
# The pairing is not mechanical and cannot be guessed from the names:
# kernel.dev.obi packages vmours.with-tests, kernel.dev.dbg_1.obi packages
# vmours, and fake.obi packages neither. It also moves: the copy that used to
# live in this script named a binary that had quietly stopped being a build
# target (docs/changes/260909/tooling.md). So kernel_image() writes an
# <image>.debug.json beside each .obi, and this reads that.
# --------------------------------------------------------------------------


def switchset_of(kernel: dict) -> str | None:
    """The directory under //build/switch/set that built this kernel."""
    label = kernel["label"]
    if SWITCH_SET not in label:
        return None
    return label.split(SWITCH_SET, 1)[1].split(":", 1)[0]


def normalise(label: str) -> str:
    """//dir -> //dir:dir, so a product's shorthand matches an image's label."""
    return label if ":" in label else f"{label}:{label.rsplit('/', 1)[-1]}"


def resolve(out: Path, product: str, arch: str, switchset: str) -> tuple[Path, Path, str]:
    # Level 1+2. A missing directory is far more often "never configured this
    # combination" than "typo", so lead with the command that creates it -- but
    # still say which products exist, because that list is the only one this
    # cannot get from the build directory it is being told does not exist.
    if not out.is_dir():
        products = sorted(p.stem for p in (ROOT / "products").glob("*.gni"))
        die(f"no build directory at {out}",
            f"configure it with:  tools/set.py {product}.{arch}",
            f"products the tree defines: {', '.join(products)}")

    info_path = out / "product_info.json"
    if not info_path.exists():
        die(f"no {info_path} -- run 'prebuilt/gn/gn gen {out}' first")

    info = json.loads(info_path.read_text())[0]
    if info["name"] != product:
        die(f"{out} was generated for product '{info['name']}', not '{product}'",
            f"re-run:  tools/set.py {product}.{arch}")

    # Level 3. The directory holds more images than the product does -- fake.obi
    # is a dependency of //:default, not of any product -- and two images can
    # share a switchset, so narrowing by the product first is what makes the
    # switchset a unique key rather than an ambiguous one.
    owned = {normalise(k["package"]) for k in info["kernel_images"]}

    # Everything the directory describes, then the product's share of it. Both
    # are kept: an image outside the product is not a candidate, but knowing it
    # exists is what lets a wrong answer say something better than "no".
    candidates: dict[str, tuple] = {}
    foreign: dict[str, tuple] = {}
    broken: list[tuple[str, list]] = []

    for manifest in sorted(glob.glob(str(out / "*.debug.json"))):
        records = json.loads(Path(manifest).read_text())
        identity = next((r for r in records if "obi" in r), None)
        if identity is None:
            continue
        kernels = [r for r in records if r.get("environment") == "kernel"]

        # Not skipped silently: an image whose kernel cannot be identified is a
        # thing someone just changed, and dropping it out of the menu without a
        # word is the worst possible way to report that.
        if len(kernels) != 1 or switchset_of(kernels[0]) is None:
            broken.append((identity["image"], kernels))
            continue

        where = candidates if identity["label"] in owned else foreign
        got = switchset_of(kernels[0])

        # Two images of one product in one switchset make the switchset
        # ambiguous, and picking either would be a coin toss the caller never
        # sees. The third level stops being a key at that point, so say so
        # rather than answer.
        if got in where:
            where[got] = ("AMBIGUOUS", where[got], (identity, kernels[0]))
        else:
            where[got] = (identity, kernels[0])

    if not candidates and not foreign:
        if broken:
            die(*([f"{out} describes no image this script can resolve; every one"
                   " of them has an ambiguous kernel:"]
                  + [f"  {name}: {len(k)} ELFs in the kernel environment"
                     + (" -- " + ", ".join(e["label"] for e in k) if k else "")
                     for name, k in broken]
                  + ["a bootable image is expected to carry exactly one kernel;"
                     " a second kernel_package would do this"]))
        die(f"{out} describes no images at all",
            f"has 'prebuilt/ninja/ninja -C {out}' been run?")

    if not candidates:
        die(f"{out} describes no image belonging to product '{product}'",
            "it does describe: "
            + ", ".join(f"{i['image']} ({s})" for s, (i, _) in sorted(foreign.items())),
            f"add one to products/{product}.gni to reach it from here")

    chosen = candidates.get(switchset)

    if chosen is not None and chosen[0] == "AMBIGUOUS":
        names = ", ".join(i["image"] for i, _ in chosen[1:])
        die(f"product '{product}' has more than one image in switchset "
            f"'{switchset}': {names}",
            "the switchset stops identifying an image once two of them share it;",
            "give one of them its own switchset, or select by image name instead")

    if chosen is None:
        have = ", ".join(f"{s} ({v[0]['image']})"
                         for s, v in sorted(candidates.items())
                         if v[0] != "AMBIGUOUS")
        lines = [f"product '{product}' has no image built in switchset '{switchset}'",
                 f"it offers: {have}"]
        if switchset in foreign:
            other = foreign[switchset]
            name = other[1][0]["image"] if other[0] == "AMBIGUOUS" else other[0]["image"]
            lines.append(f"note: {name} is built in '{switchset}' but belongs to no product")
        if broken:
            lines.append("note: ignored " + ", ".join(
                f"{n} ({len(k)} kernel ELFs)" for n, k in broken))
        die(*lines)

    identity, kernel = chosen
    return out / identity["obi"], out / kernel["debug"], identity["image"]


# --------------------------------------------------------------------------
# Booting.
# --------------------------------------------------------------------------


def clear_port(port: int) -> None:
    """Two debuggers on one port take each other down, and the survivor is not
    always the one you just started -- so a stale QEMU or lldb from an
    interrupted session has to go before this one comes up. Only processes
    actually holding the port are touched; an ordinary qemu_boot.py running
    beside this one is left alone."""
    found = subprocess.run(["lsof", "-ti", f"tcp:{port}"],
                           capture_output=True, text=True)
    stale = [int(pid) for pid in found.stdout.split()]
    if not stale:
        return
    note(f"killing stale process(es) on port {port}: "
         + " ".join(str(p) for p in stale))
    for pid in stale:
        try:
            os.kill(pid, signal.SIGTERM)
        except OSError:
            pass
    time.sleep(1)


def wait_for_stub(port: int, qemu: subprocess.Popen, deadline: float) -> None:
    """Poll rather than sleep: the ESP is rebuilt (grub-mkimage, mformat,
    mcopy) before QEMU is even exec'd, and how long that takes is not a
    constant worth guessing at."""
    while time.monotonic() < deadline:
        if qemu.poll() is not None:
            die("QEMU exited before the gdbstub came up")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.25):
                return
        except OSError:
            time.sleep(0.1)
    die(f"the gdbstub did not open on port {port} within "
        f"{int(deadline - time.monotonic() + 60)}s")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Boot an ours kernel halted with the gdbstub open.")
    parser.add_argument("product", nargs="?", default=os.environ.get("PRODUCT", "bringup"))
    parser.add_argument("arch", nargs="?", default=os.environ.get("ARCH", "x86"))
    parser.add_argument("switchset", nargs="?",
                        default=os.environ.get("SWITCHSET", "dbg.1"))
    args = parser.parse_args(argv)

    port = int(os.environ.get("PORT", "1234"))

    # The build directory is not a fourth choice, it IS the first two:
    # tools/set.py <product>.<arch> writes args.gn and runs gn gen into exactly
    # this path.
    out = Path(os.environ.get("OUT") or ROOT / f"out/{args.product}.{args.arch}")

    # The override is tested FIRST, before the build directory is even looked
    # at: booting something the build does not describe is exactly the case
    # where there is nothing to read, and requiring it would close the escape
    # hatch at the moment it is needed. The names match qemu_boot.py beside this
    # script, which already calls it IMAGE. They override together -- naming a
    # new image while keeping the build's symbols is precisely the mistake all
    # of this prevents.
    env_image, env_elf = os.environ.get("IMAGE"), os.environ.get("ELF")
    if env_image is not None or env_elf is not None:
        if env_image is None or env_elf is None:
            die("IMAGE and ELF override the build together, or neither")
        image, elf = Path(env_image), Path(env_elf)
    else:
        image, elf, name = resolve(out, args.product, args.arch, args.switchset)
        note(f"{args.product}.{args.arch} {args.switchset} -> {name}")

    # Absolute from here on. `elf` gets symlinked from out/.debug/kernel.elf
    # below; a relative ELF (from a relative $OUT, or a relative $ELF override)
    # would be stored in the link as-is and then resolved from out/.debug/'s
    # own location instead of the caller's cwd, pointing nowhere. image has no
    # such link, but is resolved alongside it so both name what they actually
    # booted regardless of the caller's cwd.
    image, elf = image.resolve(), elf.resolve()

    # Both checked before QEMU is touched. A missing ELF is the nastier of the
    # two: QEMU would boot fine and the attach would come up with no symbols at
    # all, which reads like a debugger problem rather than a missing build.
    if not image.is_file():
        die(f"no image at {image} -- build it first")
    if not elf.is_file():
        die(f"no kernel ELF at {elf} -- build it first")

    # The fixed path the debugger attaches to. Publishing it here, from the same
    # place that chose the image, is what makes "wrong symbols for this boot"
    # unrepresentable: there is one name, and whoever booted last owns it.
    #
    # Note out/.debug, NOT $OUT/.debug. $OUT moves with the product and the
    # architecture, and the debugger configuration has to name one fixed path --
    # naming a varying one would mean reading the product and architecture
    # inputs from the launch configuration as well as from the task, and VS Code
    # asks for an input once per section, so every prompt would come up twice.
    #
    # A symlink rather than a copy so a rebuild is picked up without re-running
    # this, and lldb reports the module basename as `kernel.elf` (it does not
    # resolve the link), which is what `target modules load --file` matches on.
    #
    # Published only after both checks above pass, and note what is deliberately
    # NOT done on failure: the old link is left alone. A second run that fails
    # because something is unbuilt must not break the QEMU still running from
    # the first one -- that link is still the right one for it, and
    # "Kernel: attach to a QEMU I started myself" depends on it.
    link = ROOT / "out" / ".debug" / "kernel.elf"
    link.parent.mkdir(parents=True, exist_ok=True)
    if link.is_symlink() or link.exists():
        link.unlink()
    link.symlink_to(elf)

    # The first line the VS Code task keys on; the last one is the ready line at
    # the bottom.
    note(f"booting {image}")
    note(f"symbols {elf} -> {link}")
    note(f"gdbstub on localhost:{port}")

    clear_port(port)

    # -S halts the guest before its first instruction, which is what makes a
    # breakpoint on early boot possible at all: by the time a running kernel
    # could be attached to, start_kernel is long past.
    qemu = subprocess.Popen(
        [sys.executable, str(ROOT / "scripts/qemu_boot.py"),
         "-S", "-gdb", f"tcp::{port}"],
        cwd=str(ROOT),
        env=dict(os.environ, IMAGE=str(image), OUT=str(out)),
    )

    def cleanup() -> None:
        if qemu.poll() is None:
            qemu.terminate()

    atexit.register(cleanup)
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: sys.exit(128 + sig))

    wait_for_stub(port, qemu, time.monotonic() + 60)

    # The line the VS Code task watches for. Nothing else prints it, and the
    # debugger attaches as soon as it appears -- so everything it depends on,
    # the symlink included, is already in place above.
    note(f"gdbstub ready on localhost:{port}")
    # The whole attach line, so a terminal user does not have to reconstruct it
    # -- including the script import, which F5 gets from initCommands but a
    # bare lldb does not (see .lldbinit).
    note(f"attach with:  lldb -o 'command script import scripts/lldb_ours.py' "
         f"-o 'target create out/.debug/kernel.elf' "
         f"-o 'target modules load --file kernel.elf --slide {SLIDE}' "
         f"-o 'gdb-remote localhost:{port}'")

    return qemu.wait()


if __name__ == "__main__":
    raise SystemExit(main())
