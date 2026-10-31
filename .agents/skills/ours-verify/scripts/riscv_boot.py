"""The riscv mode of boot_check.py: boot the phys chain on QEMU's virt machine
and report how far it got.

boot_check.py hands a build directory whose args.gn says target_cpu = "riscv"
to run(). It builds the boot shim OpenSBI starts and, when that builds, the
fake kernel's image (fake.obi), boots

    qemu-system-riscv64 -machine virt -m 256M -smp 1 -nographic \
        -bios default -kernel bootshim.bin [-initrd fake.obi]

and reads the serial console for the line each stage prints first (STAGES).
The check passes only at the fake kernel's "[fake-kernel] handoff ok": the
shim found physload in the initrd, physload loaded physboot, and physboot
loaded and entered the fake kernel with a handoff it accepts. Short of that
the summary names the furthest stage seen and the one expected next.

fake.obi packs physload, physboot and the fake kernel, so it builds only
once all of them build for riscv; until then the shim boots without an
initrd, and the summary says why. Nothing on riscv halts the machine, so
QEMU is killed once the verdict is in or the timeout runs out.
"""

# macOS still ships Python 3.9 as python3; this keeps the annotations lazy.
from __future__ import annotations

import collections
import os
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

# scripts/scratch_dir.py; boot_check.py, the only importer of this file, has
# put scripts/ on sys.path.
import scratch_dir

# The shim OpenSBI starts (//kernel/arch/riscv/phys/bootshim), and the image
# it is given as its initrd (//kernel/products/image/fake), both relative to
# the build directory.
SHIM = "kernel.phys_riscv.phys.dbg_0/obj/kernel/arch/riscv/phys/bootshim/bootshim.bin"
IMAGE = "fake.obi"

QEMU = "prebuilt/qemu/bin/qemu-system-riscv64"
QEMU_LIB = "prebuilt/qemu/lib"

# Seconds per boot. OpenSBI and the shim print within a second; the rest of
# the chain is a few seconds more under emulation.
TIMEOUT = 10.0
END_GRACE = 1.0    # seconds to keep reading after "handoff ok"
ERROR_GRACE = 3.0  # seconds to keep reading after a failure, for what follows

# The stages in boot order, each with the lines that show it ran, taken from
# the sources that print them. The summary reports the furthest one seen.
STAGES = [
    # The firmware's banner, "OpenSBI %s" with its version: OpenSBI is what
    # QEMU's -bios default loads.
    ("opensbi", re.compile(r"OpenSBI v\d")),
    # bootshim_main()'s first line (kernel/arch/riscv/phys/bootshim/shim.cpp).
    ("shim", re.compile(re.escape("[riscv-bootshim]"))),
    # physload: obi_main()'s first line, printed only when the shim names
    # itself in LegacyPlatformInfo::bootloader_name (kernel/phys/physload.cpp),
    # or the module loader's line for physboot, its last
    # (kernel/phys/module_loader.cpp).
    ("physload", re.compile(r"physload: loaded by '|Loaded phys module '")),
    # physboot: load_kernel_image()'s line, its first, or the line before it
    # enters the kernel, its last (kernel/phys/physboot.cpp).
    ("physboot", re.compile(r"Kernel image at 0x|handoff: jumping to kernel at ")),
    # The fake kernel's first line, and its verdict on the handoff
    # (kernel/tests/fake-kernel/fake_kernel.cpp).
    ("fake-kernel", re.compile(re.escape("[fake-kernel] entered"))),
    ("handoff ok", re.compile(re.escape("[fake-kernel] handoff ok"))),
]
PASS_STAGE = len(STAGES) - 1

# Lines that decide a failure before the timeout: the fake kernel rejecting
# the handoff, a phys panic or error (logged with the "[error]" tag), and
# OpenSBI reporting a trap it could not hand back to S-mode.
FAILURES = [
    re.compile(re.escape("[fake-kernel] FAIL:")),
    re.compile(r"\[error\]"),
    re.compile(r"sbi_trap_error: hart\d+"),
    # The phys world's trap vector (kernel/arch/riscv/phys/start.S) reports a
    # fault in the shim, physload or physboot and parks.
    re.compile(re.escape("[riscv-phys] trap:")),
]

ANSI = re.compile(r"\x1b\[[0-9;=?]*[A-Za-z]")
TAG = "riscv"


def log_dir(out: Path) -> Path:
    """A temporary directory of this build directory's own, by the rule every
    boot script follows (scripts/scratch_dir.py), under the prefix the x86
    mode uses too."""
    return scratch_dir.path("ours-verify", out)


def configure(root: Path, out: Path, product: str) -> bool:
    if (out / "build.ninja").is_file():
        return True
    print(f"configure  {out} is not configured; running tools/set.py {product} --out {out}")
    conf = subprocess.run([sys.executable, "tools/set.py", product, "--out", str(out)],
                          cwd=root, capture_output=True, text=True)
    if conf.returncode != 0:
        print(conf.stdout[-2000:] + conf.stderr[-2000:])
        print("configure  FAILED")
        return False
    return True


def ninja(root: Path, out: Path, target: str) -> tuple[bool, str, float]:
    start = time.monotonic()
    result = subprocess.run([str(root / "prebuilt/ninja/ninja"), "-C", str(out), target],
                            cwd=root, capture_output=True, text=True)
    return result.returncode == 0, result.stdout + result.stderr, time.monotonic() - start


def first_failure(output: str) -> str:
    """The first target ninja could not build and the first error under it."""
    lines = output.splitlines()
    failed = next((l for l in lines if l.startswith("FAILED:")), "")
    error = next((l for l in lines if "error:" in l), "")
    return " / ".join(l.strip()[:160] for l in (failed, error) if l)


def build(root: Path, out: Path) -> tuple[bool, Path | None]:
    """Builds the shim, which must build, and the image, which may not yet.
    Returns whether the shim built and the image to boot with, if any."""
    ok, output, elapsed = ninja(root, out, SHIM)
    if not ok:
        print("\n".join(output.splitlines()[-40:]))
        print(f"build      FAILED ({out}: {SHIM}, {elapsed:.1f}s)")
        return False, None
    print(f"build      ok ({out}: bootshim.bin, {elapsed:.1f}s)")

    # A failed build can leave an older fake.obi on disk; booting that would
    # report on code that is not this tree's, so it is not used.
    ok, output, elapsed = ninja(root, out, IMAGE)
    if not ok:
        print(f"{IMAGE:<10} not built, so the shim boots without an initrd ({elapsed:.1f}s): "
              f"{first_failure(output)}")
        return True, None
    print(f"{IMAGE:<10} ok ({elapsed:.1f}s)")
    return True, out / IMAGE


class Boot:
    """One QEMU boot of the riscv chain, read line by line as it runs."""

    def __init__(self, root: Path, shim: Path, image: Path | None, logdir: Path,
                 timeout: float):
        self.root = root
        self.shim = shim
        self.image = image
        self.log_path = logdir / f"{TAG}.log"
        self.timeout = timeout

        self.reached = -1
        self.failures: list[str] = []
        self.tail: collections.deque[str] = collections.deque(maxlen=15)
        self.passed_at: float | None = None
        self.stop_after: float | None = None  # monotonic deadline once the verdict is known
        self.timed_out = False
        self.started = 0.0
        self.proc: subprocess.Popen[bytes] | None = None
        self.reader: threading.Thread | None = None

    def command(self) -> list[str]:
        cmd = [str(self.root / QEMU), "-machine", "virt", "-m", "256M", "-smp", "1",
               "-nographic", "-bios", "default", "-kernel", str(self.shim)]
        if self.image is not None:
            cmd += ["-initrd", str(self.image)]
        return cmd

    def start(self) -> None:
        # The prebuilt QEMU's dylibs record Homebrew paths that do not exist
        # here; dyld falls back to this directory for them (scripts/qemu_boot.py).
        env = dict(os.environ, DYLD_FALLBACK_LIBRARY_PATH=str(self.root / QEMU_LIB))
        self.started = time.monotonic()
        self.proc = subprocess.Popen(self.command(), cwd=self.root, env=env,
                                     stdin=subprocess.DEVNULL,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()

    def read(self) -> None:
        assert self.proc is not None and self.proc.stdout is not None
        with open(self.log_path, "wb") as log:
            for raw in self.proc.stdout:
                log.write(raw)
                self.feed(ANSI.sub("", raw.decode("utf-8", "replace")).rstrip("\r\n"))

    def feed(self, line: str) -> None:
        now = time.monotonic()
        for i in range(self.reached + 1, len(STAGES)):
            if STAGES[i][1].search(line):
                self.reached = i
        if self.reached == PASS_STAGE and self.passed_at is None:
            self.passed_at = now - self.started
            if self.stop_after is None:
                self.stop_after = now + END_GRACE
        if any(f.search(line) for f in FAILURES):
            self.failures.append(line)
            if self.stop_after is None:
                self.stop_after = now + ERROR_GRACE
        if line.strip():
            self.tail.append(line)

    def poll(self) -> bool:
        """Kills QEMU when its time is up; returns True once it is gone."""
        assert self.proc is not None
        if self.proc.poll() is not None:
            return True
        now = time.monotonic()
        if self.stop_after is not None:
            if now >= self.stop_after:
                self.proc.kill()
        elif now - self.started >= self.timeout:
            self.timed_out = True
            self.proc.kill()
        return False

    def finish(self) -> None:
        assert self.proc is not None and self.reader is not None
        self.proc.wait()
        self.reader.join(timeout=5)

    def verdict(self) -> tuple[bool, str]:
        if self.failures:
            return False, f"{len(self.failures)} failure line(s)"
        if self.passed_at is not None:
            return True, f"handoff ok at {self.passed_at:.1f}s"
        if self.timed_out:
            return False, f'no "[fake-kernel] handoff ok" within {self.timeout:.0f}s'
        assert self.proc is not None
        return False, f"QEMU exited (rc {self.proc.returncode}) before the fake kernel's verdict"

    def report(self) -> bool:
        ok, why = self.verdict()
        stage = STAGES[self.reached][0] if self.reached >= 0 else "nothing"
        booted = f"initrd {self.image.name}" if self.image is not None else "no initrd"
        print(f"{TAG:<10} {'PASS' if ok else 'FAIL'}  {why} · reached: {stage} · {booted}")
        if ok:
            return True
        if self.reached < PASS_STAGE:
            name, marker = STAGES[self.reached + 1]
            print(f"  next stage: {name}, shown by /{marker.pattern}/")
        for title, lines in (("failure lines", self.failures[:10]),
                             ("last lines", list(self.tail))):
            if lines:
                print(f"  {title}:")
                for l in lines:
                    print(f"    {l[:200]}")
        return False


def run(root: Path, out: Path, product: str, no_build: bool, timeout: float | None) -> int:
    if not no_build:
        if not configure(root, out, product):
            return 1
        built, image = build(root, out)
        if not built:
            return 1
    else:
        image = out / IMAGE if (out / IMAGE).is_file() else None

    shim = out / SHIM
    if not shim.is_file():
        print(f"{TAG:<10} FAIL  missing shim {shim}")
        return 1
    if not (root / QEMU).is_file():
        print(f"{TAG:<10} FAIL  missing {root / QEMU}")
        return 1

    logdir = log_dir(out)
    logdir.mkdir(parents=True, exist_ok=True)
    boot = Boot(root, shim, image, logdir, TIMEOUT if timeout is None else timeout)
    boot.start()
    while not boot.poll():
        time.sleep(0.1)
    boot.finish()

    ok = boot.report()
    print(f"{'host-tests':<10} not run in the riscv mode; the x86 check runs them")
    print(f"{'headers':<10} not run in the riscv mode; the x86 check runs it, "
          f"or run scripts/header_lists.py on this build directory")
    print(f"logs       {boot.log_path}")
    return 0 if ok else 1
