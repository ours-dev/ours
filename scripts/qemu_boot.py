#!/usr/bin/env python3
"""Boot an ours kernel image under QEMU via GRUB.

QEMU's own -kernel implements Multiboot 1 only, and the phys bootshim carries a
Multiboot2 header (0xE85250D6), so it has to come up through GRUB the same way
it does on real hardware -- which is also what the design calls for:
GRUB (Multiboot2) -> bootshim -> physload -> kernel.

QEMU and the GRUB tools live in prebuilt/ (Homebrew bottles, extracted, never
installed). Their dylibs record absolute /opt/homebrew paths that do not exist
here; DYLD_FALLBACK_LIBRARY_PATH is what dyld consults when a recorded path is
missing, so pointing it at the extracted lib/ resolves them without rewriting a
single install_name.

Environment:
    OUT     build directory (default: out/bringup.x86)
    IMAGE   .obi to boot (default: $OUT/kernel.dev.obi)
    WORK    scratch directory for the ESP (default: /tmp/ours-boot)
    SMP     CPU count (default: 4; 8 under NUMA)
    MEM     megabytes (default: 512; 1024 under NUMA; just under 1 GiB,
            divided evenly, for a generated machine)
    CPU     QEMU cpu model (default: Nehalem)
    NUMA    a node count: 0 for a uniform machine, 1 for the designed
            four-node topology described below, and n >= 2 for n generated
            nodes. A generated machine exists to go past what the kernel can
            represent -- MAX_NODES is 32, so NUMA=33 is the smallest machine
            whose firmware names a proximity domain the kernel has to leave
            behind, and the only way to run the truncation paths in
            //kernel/arch/x86/mm/srat.cpp. It also declares no distances, so
            its firmware has an SRAT and no SLIT: the common shape on real
            hardware, and the one NUMA=1 cannot produce.
    REBOOT  1 to let a guest reset reboot the machine instead of ending QEMU
            (-no-reboot is passed otherwise); a panic crash record is only
            read by the boot after a reset
    DISK    a raw file to attach as a virtio-blk disk (default: none, and the
            machine has no such device). The guest finds a PCI function with
            vendor id 0x1af4 and an interrupt of its own, which is what a
            user-space block driver and the device interrupt tests need. The
            file is opened with snapshot=on: writes go to a temporary overlay
            that goes away with the machine and the file is never changed, so
            the dbg and rel images boot_check starts side by side can use one
            file (two machines cannot both hold a raw file for writing)

Anything after the options is forwarded to QEMU untouched, which is how
qemu_debug.py adds -S -gdb.
"""

from __future__ import annotations

import datetime
import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# The layout NUMA=1 builds, kept from //.legacy/ours-dev/scripts/shell/runvm.sh
# because its distance matrix is deliberately not uniform -- a kernel that only
# ever sees 10/20 distances never exercises the fallback ordering:
#
#   node 0 (cpu 0-1) --10-- node 1 (cpu 2-3)
#      |  \                     |
#      20    \30            20  |
#      |        \  30           |
#   node 2 (cpu 4-6) --10-- node 3 (cpu 7)
NUMA_NODES = [(0, "0-1"), (1, "2-3"), (2, "4-6"), (3, "7")]
NUMA_DISTANCES = [
    (0, 1, 10), (1, 0, 10), (2, 3, 10), (3, 2, 10),
    (0, 2, 20), (2, 0, 20), (1, 3, 20), (3, 1, 20),
    (0, 3, 30), (3, 0, 30), (1, 2, 30), (2, 1, 30),
]

GRUB_CFG = """set timeout=0
set default=0
terminal_output console
menuentry "ours" {
    multiboot2 /boot/bootshim
    module2 /boot/image.obi
    boot
}
"""

# grub-mkimage, not grub-mkrescue. mkrescue insists on data files (unicode.pf2,
# locales) under the pkgdatadir it was compiled with -- /opt/homebrew/... -- and
# there is no env var to redirect it. mkimage takes -d for the modules and needs
# nothing else, and the font is only for the graphical menu we do not use.
GRUB_MODULES = [
    "normal", "multiboot2", "configfile", "echo", "test", "part_gpt",
    "part_msdos", "fat", "iso9660", "serial", "terminal", "ls", "minicmd",
]


def die(message: str) -> "typing.NoReturn":  # noqa: F821
    sys.exit(f"qemu-boot: {message}")


def digest(path: Path) -> str:
    with open(path, "rb") as fd:
        return hashlib.sha256(fd.read()).hexdigest()


def identify(label: str, path: Path, sha: str) -> None:
    """Print which bytes this boot is actually running.

    This exists because of a whole wasted investigation. `out/` is shared with
    every other session working in this worktree, and a run of
    `IMAGE=out/bringup.x86/kernel.dev.obi qemu_boot.py` says nothing about
    which build of that name it got. A fault was chased as "intermittent"
    across six control runs and fourteen reproduction attempts before it turned
    out to be deterministic and already fixed: half those boots had silently
    picked up someone else's rebuild. A log that does not name its own image
    cannot be compared against another log, and no amount of care at the
    keyboard fixes that -- the identity has to be in the log.

    The digest is the part that matters. mtime alone would not have caught it:
    a rebuild that produces identical bytes moves the mtime, and two different
    builds can land in the same second.
    """
    stamp = datetime.datetime.fromtimestamp(path.stat().st_mtime)
    print(f"qemu-boot: {label} {path}", file=sys.stderr)
    print(f"qemu-boot:   {path.stat().st_size} bytes, built {stamp:%Y-%m-%d %H:%M:%S}, "
          f"sha256 {sha[:16]}", file=sys.stderr)


def build_esp(work: Path, shim: Path, image: Path, env: dict[str, str]) -> Path:
    """Assemble a bootable FAT image and return its path."""
    if work.exists():
        shutil.rmtree(work)
    esp = work / "esp"
    (esp / "EFI" / "BOOT").mkdir(parents=True)
    (esp / "boot" / "grub").mkdir(parents=True)

    shutil.copy2(shim, esp / "boot" / "bootshim")
    shutil.copy2(image, esp / "boot" / "image.obi")

    # The identity is taken from the copy, not from the source, and then
    # checked back against the source: a concurrent `ninja` rewriting the .obi
    # mid-copy would otherwise hand us a torn image and a digest that describes
    # neither half of it. Better to refuse than to boot something no log can
    # name.
    copied = digest(esp / "boot" / "image.obi")
    if copied != digest(image):
        die(f"{image} changed while it was being copied -- another build is "
            "writing it; wait for that build to finish and run this again")
    identify("booting", image, copied)
    identify("via shim", shim, digest(esp / "boot" / "bootshim"))
    (esp / "boot" / "grub" / "grub.cfg").write_text(GRUB_CFG)

    platform = ROOT / "prebuilt/grub/lib/x86_64-elf/grub/x86_64-efi"
    mkimage = subprocess.run(
        ["x86_64-elf-grub-mkimage", "-d", str(platform), "-O", "x86_64-efi",
         "-p", "/boot/grub", "-o", str(esp / "EFI/BOOT/BOOTX64.EFI")] + GRUB_MODULES,
        env=env, capture_output=True, text=True,
    )
    if mkimage.returncode != 0:
        sys.stderr.write(mkimage.stdout + mkimage.stderr)
        die("grub-mkimage failed")

    # A bare FAT volume is enough: the firmware looks for /EFI/BOOT/BOOTX64.EFI
    # on any FAT partition, and a whole-disk FAT with no partition table
    # qualifies.
    esp_img = work / "esp.img"
    with open(esp_img, "wb") as fd:
        fd.truncate(64 * 1024 * 1024)
    mtools = dict(env, MTOOLS_SKIP_CHECK="1")
    subprocess.run(["mformat", "-i", str(esp_img), "-F", "-v", "OURS", "::"],
                   env=mtools, check=True)
    subprocess.run(["mcopy", "-i", str(esp_img), "-s",
                    str(esp / "EFI"), str(esp / "boot"), "::"],
                   env=mtools, check=True)
    return esp_img


def disk_args(disk: str) -> list[str]:
    """The QEMU arguments that attach the raw file |disk| as a virtio-blk disk.

    snapshot=on is not a convenience: boot_check starts two machines at once,
    and without it the second one dies on QEMU's write lock on the file."""
    path = Path(disk).resolve()
    if not path.is_file():
        die(f"DISK={disk} is not a file")
    # A comma in a QEMU option value is written twice.
    name = str(path).replace(",", ",,")
    return ["-drive", f"file={name},if=none,id=d0,format=raw,snapshot=on",
            "-device", "virtio-blk-pci,drive=d0"]


def nvme_args(nvme_disk: str) -> list[str]:
    """The QEMU arguments that attach the raw file |nvme_disk| behind QEMU's
    NVMe controller, as the second block peripheral (the driver polls its
    completion queue; the device's interrupt pin is never unmasked)."""
    path = Path(nvme_disk).resolve()
    if not path.is_file():
        die(f"NVME_DISK={nvme_disk} is not a file")
    name = str(path).replace(",", ",,")
    return ["-drive", f"file={name},if=none,id=nvme0,format=raw,snapshot=on",
            "-device", "nvme,drive=nvme0,serial=ours0"]


def numa_args(mem: int, nodes: int, smp: int, designed: bool) -> list[str]:
    node_mem, remainder = divmod(mem, nodes)
    if remainder:
        die(f"MEM={mem} does not divide into {nodes} NUMA nodes")

    args = []
    # Every node needs its own backend object: with -numa node,mem=... QEMU only
    # splits the -m total, and it has been deprecated for exactly the reason it
    # matters here -- a backend is what gives the guest a real ACPI SRAT entry.
    for node in range(nodes):
        args += ["-object", f"memory-backend-ram,size={node_mem}M,id=m{node}"]

    # The designed machine splits its eight CPUs 2/2/3/1 to match the diagram
    # above. A generated one gives the first nodes one CPU each and leaves the
    # rest memory-only, which is legal and is what a machine with far more
    # proximity domains than CPUs looks like -- and it keeps every CPU's domain
    # inside MAX_NODES, so what a NUMA=33 boot truncates is memory, not CPUs.
    layout = NUMA_NODES if designed else [
        (node, str(node) if node < smp else "") for node in range(nodes)]
    for node, cpus in layout:
        cpu_arg = f",cpus={cpus}" if cpus else ""
        args += ["-numa", f"node,nodeid={node}{cpu_arg},memdev=m{node}"]

    # Both directions spelled out: QEMU mirrors a single direction on its own,
    # but saying it twice keeps the matrix readable next to the diagram above.
    # A generated machine is given none, and QEMU then builds no SLIT at all:
    # its firmware names the proximity domains and not their distances, which
    # is the common shape on real hardware and one NUMA=1 cannot produce.
    if designed:
        for src, dst, val in NUMA_DISTANCES:
            args += ["-numa", f"dist,src={src},dst={dst},val={val}"]
    return args


def main(argv: list[str]) -> int:
    out = Path(os.environ.get("OUT") or ROOT / "out/bringup.x86")
    image = Path(os.environ.get("IMAGE") or out / "kernel.dev.obi")
    work = Path(os.environ.get("WORK") or "/tmp/ours-boot")
    shim = out / ("kernel.phys_x86.phys.dbg_0/obj/kernel/arch/x86/phys/"
                  "bootshim/bootshim")

    if not shim.is_file():
        die(f"missing shim: {shim}")
    if not image.is_file():
        die(f"missing image: {image}")

    env = dict(os.environ)
    env["DYLD_FALLBACK_LIBRARY_PATH"] = (
        f"{ROOT}/prebuilt/qemu/lib:{ROOT}/prebuilt/grub/lib")
    env["PATH"] = f"{ROOT}/prebuilt/grub/bin:{env.get('PATH', '')}"

    esp_img = build_esp(work, shim, image, env)

    firmware = ROOT / "prebuilt/qemu/share/qemu"
    shutil.copy2(firmware / "edk2-x86_64-code.fd", work / "code.fd")
    shutil.copy2(firmware / "edk2-i386-vars.fd", work / "vars.fd")

    # A count, not a flag: NUMA=1 still means the designed four-node machine,
    # and anything above it is that many generated nodes.
    numa = int(os.environ.get("NUMA") or 0)
    designed = numa == 1
    nodes = len(NUMA_NODES) if designed else numa
    # Defaults that make the topology above come out whole: 8 CPUs to split
    # 2/2/3/1, and memory that divides by four.
    #
    # A generated machine splits as close to 1 GiB as its node count divides
    # into. Small on purpose: it exists for its node count, not its memory.
    # 16M a node is the floor, since a node has to hold more than the
    # bookkeeping for itself.
    generated_mem = max(16, 1024 // nodes) * nodes if nodes else 0
    default_mem = 512 if not numa else (1024 if designed else generated_mem)
    mem = int(os.environ.get("MEM") or default_mem)
    # Four CPUs by default, not one. The kernel brings up every CPU the MADT
    # reports and spreads threads over them, and almost none of that code is
    # exercised by a uniprocessor boot: the 16-bit trampoline, the per-CPU
    # areas, the reschedule IPIs, the remote run-queue inserts, the local
    # APIC's own software enable. Every one of those held a bug a single-CPU
    # boot could not see. Pass SMP=1 to compare against a uniprocessor run when
    # a failure might be an SMP race.
    smp = int(os.environ.get("SMP") or (8 if numa else 4))

    # CPU model matters: the kernel is built -march=x86-64-v2, and QEMU's
    # default qemu64 model is roughly v1 -- it does not advertise POPCNT, so the
    # first popcntq the compiler emits raises #UD. Nehalem is the canonical v2
    # baseline.
    qemu = ROOT / "prebuilt/qemu/bin/qemu-system-x86_64"
    disk = os.environ.get("DISK")
    nvme_disk = os.environ.get("NVME_DISK")
    cmd = [
        str(qemu),
        "-M", "q35", "-cpu", os.environ.get("CPU", "Nehalem"),
        "-m", str(mem), "-smp", str(smp),
        *(numa_args(mem, nodes, smp, designed) if numa else []),
        *([] if os.environ.get("REBOOT") == "1" else ["-no-reboot"]),
        "-nographic",
        "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={work}/code.fd",
        "-drive", f"if=pflash,format=raw,unit=1,file={work}/vars.fd",
        "-drive", f"format=raw,file={esp_img}",
        *(disk_args(disk) if disk else []),
        *(nvme_args(nvme_disk) if nvme_disk else []),
        "-serial", "mon:stdio",
        *argv,
    ]

    # execv rather than subprocess: this process has nothing left to do, and
    # callers (qemu_debug.py, VS Code's task terminal) send signals to the pid
    # they started. Staying alive in between would swallow them.
    sys.stdout.flush()
    os.execve(str(qemu), cmd, env)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
