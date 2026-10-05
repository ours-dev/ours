#!/usr/bin/env python3
"""Build the ours kernel, boot both dev images under QEMU, and judge each boot.

Run from the repo root:

    python3 .agents/skills/ours-verify/scripts/boot_check.py
    python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/mine
    python3 .agents/skills/ours-verify/scripts/boot_check.py --only dbg --no-build

A boot passes when the shell prints its closing "--- end ---" line, every
line the image requires (REQUIRED) appeared, no "FAILED" line appeared, and
nothing logged at error level -- panic() and a failed ASSERT both log at error
level before halting. The kernel never exits,
so each QEMU is killed as soon as its verdict is known or the timeout runs
out. Full serial logs stay on disk; only a summary is printed.

On dev (rel), once the PS/2 keyboard driver says it is ready, keys are typed
into the machine through QEMU's monitor (KEYS), and the driver's line for
each is among the required ones: a key reaches a user-space driver through
the keyboard's interrupt line, end to end.

Environment variables such as SMP=1 or NUMA=1 pass straight through to
scripts/qemu_boot.py.
"""

# macOS still ships Python 3.9 as python3; this keeps the annotations lazy.
from __future__ import annotations

import argparse
import collections
import os
import re
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

IMAGES = {
    # DEBUG_ASSERT compiled in: stops at the first failed assertion.
    "dbg": "kernel.dev.dbg_1.obi",
    # Assertions compiled out: runs on past the same bug, so it often crashes
    # somewhere else. Both images failing at the same point is evidence that
    # no assertion is masking anything.
    "rel": "kernel.dev.obi",
}

END_MARKER = "--- end ---"  # the last line examples/shell/shell.cpp prints

# Lines an image must print besides the end marker, matched as substrings of a
# line. They come from other processes than the shell, so they may arrive
# after END_MARKER: the reader keeps going until the marker and every one of
# them have appeared, or the timeout runs out. On dev (rel), init is the root
# of the component tree and starts routing_checks, which checks the namespace
# it was given and says so (.agents/tasks/component-framework/PLAN.md, step
# 13), and echo, which routing_checks reaches through its sibling route and
# asks to stop, and whose exit the root reports (step 17). The root also says
# it holds the roots of the interrupt lines and I/O ports that userboot hands
# on (.agents/tasks/microkernel-interrupts/PLAN.md, step 9); dbg_1 has no
# root, and its shell checks those resources itself. And the root starts the
# PS/2 keyboard driver, which says when it holds the keyboard's line and
# then logs each key KEYS types (step 12).
REQUIRED = {
    "dbg": [],
    "rel": [
        "routing_checks: passed",
        "init: echo exited with 0",
        "init: holding the IRQ and I/O port resources",
        "pc-ps2: ready",
        "pc-ps2: key a",
        "pc-ps2: key A",
    ],
}

# Keys typed into an image once a line says its keyboard driver is ready:
# that line, then each key as QEMU's `sendkey` names it. COM1 and QEMU's
# monitor share stdio (`-serial mon:stdio`, scripts/qemu_boot.py), and
# Ctrl-A c moves the input between them, so the keys go to the monitor
# between two of those and the console sees none of it. The monitor's
# `sendkey` presses each key and releases it, a modifier first and last:
# "a" is the driver's lower-case path, "shift-a" its shift state. Only dev
# (rel) has the driver: it is a component of the root.
KEYS = {
    "dbg": None,
    "rel": ("pc-ps2: ready", ["a", "shift-a"]),
}
MONITOR_TOGGLE = b"\x01c"  # Ctrl-A c
ANSI = re.compile(r"\x1b\[[0-9;=?]*[A-Za-z]")
# Log tags come as "[error]" (phys), "[123 error]" (early kernel, before
# per-CPU state) and "[123 cpu0 error]". Searched, not anchored: early serial
# output can glue stray bytes onto the front of a line. "Exception[divide
# error]" from a user fault test does not match.
LEVEL = r"\[(?:\d+ )?(?:cpu\d+ )?(?:{})\]"
ERROR_LINE = re.compile(LEVEL.format("error"))
FAILED_LINE = re.compile(r"\bFAILED\b")
# Hidden from the printed context. The dbg image logs "I'm Thread N" at debug
# level forever, several thousand lines a second.
NOISE = re.compile(LEVEL.format("debug|trace"))
LTO_WARNING = "ignoring -lto_library"  # host linker noise, harmless

# Boot stages in order; the summary reports the furthest one reached.
MILESTONES = [
    ("firmware", re.compile(r"Booting `ours'")),
    ("phys", re.compile(r"boot-shim:")),
    ("kernel", re.compile(r"\[\d+ (?:cpu\d+ )?(?:info|debug|trace|warn|error)\]")),
    ("userboot", re.compile(r"^userboot: start")),
    ("init", re.compile(r"^init: started")),
    ("shell", re.compile(r"^shell: ")),
    ("end", re.compile(re.escape(END_MARKER))),
]

END_GRACE = 1.0    # seconds to keep reading after the end marker
ERROR_GRACE = 3.0  # seconds to keep reading after an error, for the dump


def repo_root() -> Path:
    try:
        top = subprocess.run(["git", "rev-parse", "--show-toplevel"],
                             check=True, capture_output=True, text=True)
        return Path(top.stdout.strip())
    except (OSError, subprocess.CalledProcessError):
        # .agents/skills/ours-verify/scripts/boot_check.py -> repo root
        return Path(__file__).resolve().parents[4]


def build(root: Path, out: Path, product: str) -> bool:
    if not (out / "build.ninja").is_file():
        print(f"configure  {out} is not configured; running tools/set.py {product} --out {out}")
        conf = subprocess.run([sys.executable, "tools/set.py", product, "--out", str(out)],
                              cwd=root, capture_output=True, text=True)
        if conf.returncode != 0:
            print(conf.stdout[-2000:] + conf.stderr[-2000:])
            print("configure  FAILED")
            return False

    start = time.monotonic()
    ninja = subprocess.run([str(root / "prebuilt/ninja/ninja"), "-C", str(out)],
                           cwd=root, capture_output=True, text=True)
    elapsed = time.monotonic() - start
    if ninja.returncode != 0:
        lines = [l for l in (ninja.stdout + ninja.stderr).splitlines() if LTO_WARNING not in l]
        print("\n".join(lines[-40:]))
        print(f"build      FAILED ({out}, {elapsed:.1f}s)")
        return False
    print(f"build      ok ({out}, {elapsed:.1f}s)")
    return True


class Boot:
    def __init__(self, tag: str, root: Path, out: Path, logdir: Path, timeout: float):
        self.tag = tag
        self.root = root
        self.out = out
        self.image = out / IMAGES[tag]
        self.log_path = logdir / f"{tag}.log"
        self.work = logdir / f"esp-{tag}"
        self.timeout = timeout

        self.reached = -1
        self.passed: list[str] = []
        self.skipped: list[str] = []
        self.failed: list[str] = []
        self.errors: list[str] = []
        self.after_error: list[str] = []
        self.tail: collections.deque[str] = collections.deque(maxlen=15)
        self.required = list(REQUIRED[tag])
        self.seen: set[str] = set()
        self.keys = KEYS[tag]
        self.typed = False
        self.end_at: float | None = None
        self.stop_after: float | None = None  # monotonic deadline once verdict is known
        self.timed_out = False
        self.proc: subprocess.Popen[bytes] | None = None
        self.reader: threading.Thread | None = None

    def start(self) -> None:
        env = dict(os.environ, OUT=str(self.out), IMAGE=str(self.image), WORK=str(self.work))
        self.started = time.monotonic()
        # qemu_boot.py execs QEMU, so this pid becomes QEMU itself. Its stdin
        # is a pipe only where keys are to be typed into it.
        stdin = subprocess.PIPE if self.keys else subprocess.DEVNULL
        self.proc = subprocess.Popen([sys.executable, "scripts/qemu_boot.py"],
                                     cwd=self.root, env=env,
                                     stdin=stdin,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()

    def type_keys(self) -> None:
        """Types the image's KEYS through QEMU's monitor, once."""
        assert self.proc is not None and self.proc.stdin is not None and self.keys is not None
        self.typed = True
        commands = "".join(f"sendkey {key}\n" for key in self.keys[1]).encode()
        try:
            self.proc.stdin.write(MONITOR_TOGGLE + commands + MONITOR_TOGGLE)
            self.proc.stdin.flush()
        except OSError:
            # QEMU has gone already: the verdict says why, and the keys'
            # lines are missing from it.
            pass

    def read(self) -> None:
        assert self.proc is not None and self.proc.stdout is not None
        with open(self.log_path, "wb") as log:
            for raw in self.proc.stdout:
                log.write(raw)
                self.feed(ANSI.sub("", raw.decode("utf-8", "replace")).rstrip("\r\n"))

    def feed(self, line: str) -> None:
        now = time.monotonic()
        for i in range(self.reached + 1, len(MILESTONES)):
            if MILESTONES[i][1].search(line):
                self.reached = i
        if "checks passed" in line:
            self.passed.append(line)
        elif line.startswith("shell:") and "skipped" in line:
            self.skipped.append(line)
        if FAILED_LINE.search(line):
            self.failed.append(line)
        if self.errors and len(self.after_error) < 25:
            self.after_error.append(line)
        if ERROR_LINE.search(line):
            self.errors.append(line)
            if self.stop_after is None:
                self.stop_after = now + ERROR_GRACE
        for required in self.required:
            if required in line:
                self.seen.add(required)
        if self.keys and not self.typed and self.keys[0] in line:
            self.type_keys()
        if END_MARKER in line and self.end_at is None:
            self.end_at = now - self.started
        # Done once the shell has finished and every required line is in,
        # whichever came last -- or, once it has finished, as soon as a
        # FAILED line has decided the verdict: a required line may then never
        # come (routing_checks prints "passed" only when nothing failed), and
        # waiting for it would only run the timeout out.
        if (self.end_at is not None and self.stop_after is None
                and (not self.missing() or self.failed)):
            self.stop_after = now + END_GRACE
        if line.strip() and not NOISE.search(line):
            self.tail.append(line)

    def missing(self) -> list[str]:
        """The required lines that have not appeared yet."""
        return [r for r in self.required if r not in self.seen]

    def poll(self) -> bool:
        """Kill QEMU when its time is up; return True once it is gone."""
        assert self.proc is not None
        if self.proc.poll() is not None:
            return True
        now = time.monotonic()
        if self.stop_after is not None and now >= self.stop_after:
            self.proc.kill()
        elif now - self.started >= self.timeout:
            self.timed_out = self.end_at is None or bool(self.missing())
            self.proc.kill()
        return False

    def finish(self) -> None:
        assert self.proc is not None and self.reader is not None
        self.proc.wait()
        self.reader.join(timeout=5)

    def verdict(self) -> tuple[bool, str]:
        if self.errors:
            return False, "error-level line (panic or failed ASSERT)"
        if self.failed:
            return False, f"{len(self.failed)} FAILED line(s)"
        if self.end_at is None:
            if self.timed_out:
                return False, f'no "{END_MARKER}" within {self.timeout:.0f}s (hung or looping)'
            return False, f"QEMU exited (rc {self.proc.returncode}) before the shell finished -- reset or triple fault?"
        if self.missing():
            lines = ", ".join(f'"{r}"' for r in self.missing())
            return False, f"no {lines} within {self.timeout:.0f}s"
        return True, f"shell finished at {self.end_at:.1f}s"

    def report(self) -> bool:
        ok, why = self.verdict()
        stage = MILESTONES[self.reached][0] if self.reached >= 0 else "nothing"
        required = ""
        if self.required:
            required = f" · {len(self.seen)}/{len(self.required)} required"
        print(f"{self.tag:<10} {'PASS' if ok else 'FAIL'}  {why} · "
              f"{len(self.passed)} passed, {len(self.skipped)} skipped, {len(self.failed)} failed"
              f"{required} · {len(self.errors)} error line(s) · reached: {stage}")
        if not ok:
            for title, lines in (("FAILED lines", self.failed[:10]),
                                 ("error lines", self.errors[:5]),
                                 ("after the first error", self.after_error),
                                 ("last lines (debug/trace hidden)", list(self.tail))):
                if lines:
                    print(f"  {title}:")
                    for l in lines:
                        print(f"    {l[:200]}")
        return ok


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", default="out/bringup.x86", help="GN build directory (default: out/bringup.x86)")
    parser.add_argument("--product", default="bringup.x86", help="product.arch for tools/set.py if --out is unconfigured")
    parser.add_argument("--no-build", action="store_true", help="boot what is already built")
    parser.add_argument("--only", choices=sorted(IMAGES), help="boot one image instead of both")
    parser.add_argument("--timeout", type=float, default=60, help="seconds per boot (default: 60)")
    args = parser.parse_args(argv)

    root = repo_root()
    out = (root / args.out).resolve()
    if not args.no_build and not build(root, out, args.product):
        return 1

    logdir = Path(tempfile.gettempdir()) / f"ours-verify-{out.name}"
    logdir.mkdir(parents=True, exist_ok=True)
    boots = [Boot(tag, root, out, logdir, args.timeout)
             for tag in IMAGES if args.only in (None, tag)]
    for b in boots:
        if not b.image.is_file():
            print(f"{b.tag:<10} FAIL  missing image {b.image}")
            return 1

    for b in boots:
        b.start()
    pending = list(boots)
    while pending:
        pending = [b for b in pending if not b.poll()]
        time.sleep(0.2)
    for b in boots:
        b.finish()

    results = [b.report() for b in boots]
    print(f"logs       {logdir}/{{{','.join(b.tag for b in boots)}}}.log")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
