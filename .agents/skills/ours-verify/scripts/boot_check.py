#!/usr/bin/env python3
"""Build the ours kernel, run its host tests, boot both dev images under QEMU,
and judge each.

Run from the repo root:

    python3 .agents/skills/ours-verify/scripts/boot_check.py
    python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/mine
    python3 .agents/skills/ours-verify/scripts/boot_check.py --only dbg --no-build

The host tests are every executable the default build compiles in the host
toolchain with testonly = true, asked of GN on each run (HostTests). They run
while the images boot, and one failing fails the check like a failing boot.

A boot passes when the shell prints its closing "--- end ---" line, every
line the image requires (REQUIRED) appeared, no "FAILED" line appeared, and
nothing logged at error level -- panic() and a failed ASSERT both log at error
level before halting. The kernel never exits,
so each QEMU is killed as soon as its verdict is known or the timeout runs
out. Full serial logs stay on disk; only a summary is printed.

On dev (rel), once the PS/2 keyboard driver says it is ready, keys are typed
into the machine through a QEMU monitor of its own (KEYS), and the driver's
line for each is among the required ones: a key reaches a user-space driver
through the keyboard's interrupt line, end to end.

A boot whose console falls silent for SILENCE seconds between the kernel's
first line and the end marker has stopped: dbg logs thousands of lines a
second while any CPU runs, and rel's whole boot takes a few seconds. Every
image gets a QEMU monitor of its own, and it is then sent `nmi`, once. The
NMI reaches the boot CPU through LINT1, whatever its interrupt flag, and the
lockup detector (//kernel/lib/lockup_detector) takes an NMI that nothing in
the kernel sent as a request for every CPU's state: it prints the lockup
report and panics. The script reads on until the panic has replayed the
ring, or REPORT_GRACE runs out, and the verdict says the boot went silent and
names the report, which is printed and stays in the log. A boot already
quiet when its timeout comes is not killed until its silence has run its
course. A lockup report the detector makes by itself, for a CPU that stopped
taking its tick, is named in the verdict too.

Environment variables such as SMP=1 or NUMA=1 pass straight through to
scripts/qemu_boot.py.
"""

# macOS still ships Python 3.9 as python3; this keeps the annotations lazy.
from __future__ import annotations

import argparse
import collections
import concurrent.futures
import os
import re
import socket
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

END_MARKER = "--- end ---"  # the last line system/app/shell/shell.cpp prints

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
# that line, then each key as QEMU's `sendkey` names it. The monitor's
# `sendkey` presses each key and releases it, a modifier first and last:
# "a" is the driver's lower-case path, "shift-a" its shift state. Only dev
# (rel) has the driver: it is a component of the root.
#
# They go to a monitor of their own, on a socket (Boot.start), and not to
# the one `-serial mon:stdio` puts behind Ctrl-A c (scripts/qemu_boot.py).
# That one shares stdout with COM1 and echoes each byte typed into it as a
# write of its own, which can fall between two of the writes, one per
# character, of a line the guest prints meanwhile. The line then has the
# echo inside it and no longer matches: the first key's own line is printed
# about when the second command is echoed, and a FAILED line split that way
# would let a failing boot pass.
KEYS = {
    "dbg": None,
    "rel": ("pc-ps2: ready", ["a", "shift-a"]),
}
# Seconds of console silence, after the kernel's first line and before the
# end marker, after which QEMU's monitor is sent `nmi`: the hang hunt's own
# measure (docs/repairs/261007/kernel-smp.md), and more than the lockup
# detector's 10 s threshold, so that a hang the detector can see from inside
# is reported by it first.
SILENCE = 15.0
# Seconds after the `nmi` for the report and the panic after it: the reporter
# waits up to 2 s for the other CPUs, and the report and the panic's replay of
# the ring take a few seconds more on the serial port.
REPORT_GRACE = 15.0
# Seconds of quiet that, when the timeout comes before the end marker, hold
# the timeout off until the quiet is a silence of SILENCE seconds and the
# report has come: dbg's boot takes most of the timeout, and a hang late in
# it would otherwise be killed before it could be asked for a report.
QUIET = 2.0
# The lockup report's first line, which says why it was made, and the first
# line of each CPU's block in it (//kernel/lib/lockup_detector/src/report.cpp).
# A CPU's register lines follow its first line; the report ends at the panic
# banner (kernel/top/panic.cpp), and the panic's replay of the ring, which
# repeats it, ends at RECENT_END.
REPORT_HEADER = re.compile(r"lockup: (?:cpu\d+ took no tick for \d+ s, found by cpu\d+"
                           r"|dump asked for by an NMI on cpu\d+)")
REPORT_BLOCK = re.compile(r"lockup: cpu\d+(?: \([^)]*\))? thread ")
PANIC_BANNER = "OURS KERNEL PANIC"
RECENT_END = "--- end of recent log ---"
REPORT_LINES = 80  # the most of a report kept for the summary; the log has it all
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
# From the kernel's first line there is a kernel to answer an NMI, and a
# console that should not fall silent.
KERNEL_STAGE = [name for name, _ in MILESTONES].index("kernel")

# The host tests are the executables GN marks testonly, in the host toolchain,
# that //:default builds: testonly is GN's own mark for a test, since only a
# testonly target may depend on one. Benchmarks are testonly too, but they
# report timings rather than pass or fail, so //benches is left out.
HOST_TOOLCHAIN = "//build/toolchain/ours:host"
HOST_TEST_EXCLUDED = ("//benches/",)
HOST_TEST_JOBS = 4       # beside two QEMUs; all of them take about 10s at this
HOST_TEST_TIMEOUT = 300  # seconds for one test binary

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


class HostTests:
    """Runs the host tests on a thread of its own, beside the boots.

    The list comes from GN rather than from this file, so a test is run as
    soon as it is in //:default: the testonly executables of the host
    toolchain (`gn ls`), those of them the default build reaches (`gn desc
    //:default deps --all`), and the file each one links (`gn outputs`).
    Each runs from the build directory, as ninja would run it, and passes
    when it exits 0.
    """

    def __init__(self, root: Path, out: Path, logdir: Path):
        self.root = root
        self.out = out
        self.log_path = logdir / "host-tests.log"
        self.labels: list[str] = []
        self.problem: str | None = None  # why the tests could not be found or run
        self.failures: list[tuple[str, str, str]] = []  # label, status, output
        self.count = 0
        self.elapsed = 0.0
        self.thread: threading.Thread | None = None

    def gn(self, *args: str) -> list[str]:
        result = subprocess.run([str(self.root / "prebuilt/gn/gn"), *args],
                                cwd=self.root, capture_output=True, text=True)
        if result.returncode != 0:
            raise RuntimeError(f"gn {args[0]}: {(result.stdout + result.stderr).strip()[-500:]}")
        return [line.strip() for line in result.stdout.splitlines() if line.strip()]

    def find(self) -> list[tuple[str, Path]]:
        suffix = f"({HOST_TOOLCHAIN})"
        tests = {label for label in self.gn("ls", str(self.out), "--testonly=true",
                                            "--type=executable")
                 if label.endswith(suffix) and not label.startswith(HOST_TEST_EXCLUDED)}
        built = set(self.gn("desc", str(self.out), "//:default", "deps", "--all"))
        labels = sorted(tests & built)
        if not labels:
            return []
        outputs = self.gn("outputs", str(self.out), *labels)
        if len(outputs) != len(labels):
            raise RuntimeError(f"gn outputs gave {len(outputs)} files for {len(labels)} tests")
        return [(label, self.out / output) for label, output in zip(labels, outputs)]

    def run_one(self, test: tuple[str, Path]) -> tuple[str, str, str]:
        label, binary = test
        try:
            result = subprocess.run([str(binary)], cwd=self.out, capture_output=True,
                                    text=True, errors="replace", timeout=HOST_TEST_TIMEOUT)
        except subprocess.TimeoutExpired:
            return label, f"no exit within {HOST_TEST_TIMEOUT}s", ""
        except OSError as error:
            return label, f"cannot run: {error}", ""
        status = "ok" if result.returncode == 0 else f"exit {result.returncode}"
        return label, status, result.stdout + result.stderr

    def run(self) -> None:
        start = time.monotonic()
        try:
            tests = self.find()
        except RuntimeError as error:
            self.problem = str(error)
            return
        self.count = len(tests)
        with concurrent.futures.ThreadPoolExecutor(HOST_TEST_JOBS) as pool:
            results = list(pool.map(self.run_one, tests))
        self.elapsed = time.monotonic() - start
        self.failures = [r for r in results if r[1] != "ok"]
        with open(self.log_path, "w") as log:
            for label, status, output in results:
                log.write(f"=== {label}: {status}\n{output}\n")

    def start(self) -> None:
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def finish(self) -> None:
        assert self.thread is not None
        self.thread.join()

    def report(self) -> bool:
        if self.problem is not None:
            print(f"{'host-tests':<10} FAIL  {self.problem}")
            return False
        if self.count == 0:
            print(f"{'host-tests':<10} FAIL  GN found no host tests in //:default")
            return False
        passed = self.count - len(self.failures)
        ok = not self.failures
        print(f"{'host-tests':<10} {'PASS' if ok else 'FAIL'}  {passed} passed, "
              f"{len(self.failures)} failed · {self.elapsed:.1f}s · log {self.log_path}")
        for label, status, output in self.failures[:10]:
            print(f"  {label}: {status}")
            for line in output.strip().splitlines()[-8:]:
                print(f"    {line[:200]}")
        return ok


class Boot:
    def __init__(self, tag: str, root: Path, out: Path, logdir: Path, timeout: float):
        self.tag = tag
        self.root = root
        self.out = out
        self.image = out / IMAGES[tag]
        self.log_path = logdir / f"{tag}.log"
        self.monitor_log_path = logdir / f"{tag}.monitor.log"
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
        self.monitor: socket.socket | None = None  # this end of the image's monitor
        self.last_line = 0.0  # when the console last said anything
        self.nmi_at: float | None = None  # when the monitor was sent `nmi`, once
        self.nmi_quiet = 0.0  # how long the console had been silent then
        self.lockup_report: list[str] = []  # the lockup report, from its header to the banner
        self.in_report = False
        self.end_at: float | None = None
        self.stop_after: float | None = None  # monotonic deadline once verdict is known
        self.timed_out = False
        self.proc: subprocess.Popen[bytes] | None = None
        self.reader: threading.Thread | None = None

    def start(self) -> None:
        env = dict(os.environ, OUT=str(self.out), IMAGE=str(self.image), WORK=str(self.work))
        self.started = time.monotonic()
        self.last_line = self.started
        # QEMU gets a monitor on one end of a socket pair, a descriptor it
        # inherits (qemu_boot.py passes extra arguments on to QEMU), and this
        # script keeps the other end, to type keys into on rel and `nmi` into
        # on a silent boot. Nothing the monitor writes reaches the serial log.
        self.monitor, far = socket.socketpair()
        qemu_args = ["-chardev", f"socket,id=monitor,fd={far.fileno()}",
                     "-monitor", "chardev:monitor"]
        # qemu_boot.py execs QEMU, so this pid becomes QEMU itself, and the
        # inherited descriptor stays open across the exec.
        self.proc = subprocess.Popen([sys.executable, "scripts/qemu_boot.py", *qemu_args],
                                     cwd=self.root, env=env,
                                     stdin=subprocess.DEVNULL, pass_fds=[far.fileno()],
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        # QEMU's alone from here, so its exit is the monitor's end of file.
        far.close()
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()

    def type_keys(self) -> None:
        """Types the image's KEYS through its monitor, once."""
        assert self.monitor is not None and self.keys is not None
        self.typed = True
        commands = "".join(f"sendkey {key}\n" for key in self.keys[1]).encode()
        try:
            self.monitor.sendall(commands)
        except OSError:
            # QEMU has gone already: the verdict says why, and the keys'
            # lines are missing from it.
            pass

    def send_nmi(self, now: float) -> None:
        """Sends QEMU's monitor `nmi` once the console has been silent for
        SILENCE seconds, between the kernel's first line and the end marker,
        once per boot, and not once a verdict is in: a panic under way has
        its own report to finish. The boot is then given REPORT_GRACE for the
        report, in place of its timeout."""
        assert self.monitor is not None
        if (self.nmi_at is not None or self.reached < KERNEL_STAGE or self.end_at is not None
                or self.stop_after is not None):
            return
        quiet = now - self.last_line
        if quiet < SILENCE:
            return
        # The deadline before the mark: feed() reads the mark on its own
        # thread, and moves the deadline once the mark is set.
        self.stop_after = now + REPORT_GRACE
        self.nmi_quiet = quiet
        self.nmi_at = now
        try:
            self.monitor.sendall(b"nmi\n")
        except OSError:
            # QEMU has gone already, which the next poll() sees.
            pass

    def falling_silent(self, now: float) -> bool:
        """Whether the console has been quiet for QUIET seconds before the
        end marker, with no `nmi` sent yet: a silence that send_nmi() will
        turn into a report if it lasts, which the timeout waits for."""
        return (self.nmi_at is None and self.end_at is None and self.reached >= KERNEL_STAGE
                and now - self.last_line >= QUIET)

    def read(self) -> None:
        assert self.proc is not None and self.proc.stdout is not None
        with open(self.log_path, "wb") as log:
            for raw in self.proc.stdout:
                log.write(raw)
                self.feed(ANSI.sub("", raw.decode("utf-8", "replace")).rstrip("\r\n"))

    def feed(self, line: str) -> None:
        now = time.monotonic()
        self.last_line = now
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
        self.keep_report(line)
        if self.nmi_at is not None and RECENT_END in line:
            # The panic after the report has replayed the ring: what is left
            # is the crash record and the reset.
            self.stop_after = min(self.stop_after, now + END_GRACE)
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

    def keep_report(self, line: str) -> None:
        """Keeps the lockup report's lines, from its header to the panic
        banner, for the summary."""
        if not self.lockup_report and REPORT_HEADER.search(line):
            self.in_report = True
        if PANIC_BANNER in line:
            self.in_report = False
        if self.in_report and len(self.lockup_report) < REPORT_LINES and line.strip() \
                and not NOISE.search(line):
            self.lockup_report.append(line)

    def missing(self) -> list[str]:
        """The required lines that have not appeared yet."""
        return [r for r in self.required if r not in self.seen]

    def poll(self) -> bool:
        """Kill QEMU when its time is up; return True once it is gone."""
        assert self.proc is not None
        if self.proc.poll() is not None:
            return True
        now = time.monotonic()
        self.send_nmi(now)
        # Once a deadline is set, it is the one that counts, past the
        # timeout too: the grace for a report that an `nmi` asked for, or
        # the dump after an error, is not cut short.
        if self.stop_after is not None:
            if now >= self.stop_after:
                self.proc.kill()
        elif now - self.started >= self.timeout and not self.falling_silent(now):
            self.timed_out = self.end_at is None or bool(self.missing())
            self.proc.kill()
        return False

    def finish(self) -> None:
        assert self.proc is not None and self.reader is not None
        self.proc.wait()
        self.reader.join(timeout=5)
        assert self.monitor is not None
        # QEMU is gone, so what its monitor answered ends here. It is kept
        # beside the serial log: a key or an `nmi` QEMU refused says why in it.
        with self.monitor, open(self.monitor_log_path, "wb") as log:
            self.monitor.settimeout(1)
            try:
                while chunk := self.monitor.recv(4096):
                    log.write(chunk)
            except OSError:
                pass

    def lockup_summary(self) -> str:
        """What the lockup report says about itself: its header and how many
        CPU blocks it has. Only for a report that came."""
        # The report is kept from its header on.
        header = REPORT_HEADER.search(self.lockup_report[0])
        assert header is not None
        blocks = sum(1 for l in self.lockup_report if REPORT_BLOCK.search(l))
        return f"lockup report, {header.group(0)}, {blocks} CPU blocks"

    def verdict(self) -> tuple[bool, str]:
        if self.nmi_at is not None:
            what = f"no lockup report within {REPORT_GRACE:.0f}s"
            if self.lockup_report:
                what = f"{self.lockup_summary()} (printed below, kept in the log)"
            return False, (f"console silent for {self.nmi_quiet:.0f}s before the end, so QEMU was "
                           f"sent `nmi`: {what}")
        if self.lockup_report:
            return False, f"{self.lockup_summary()} (printed below, kept in the log)"
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
            sections = [("FAILED lines", self.failed[:10]),
                        ("error lines", self.errors[:5]),
                        ("after the first error", self.after_error),
                        ("last lines (debug/trace hidden)", list(self.tail))]
            if self.lockup_report:
                # The report is the error lines and more, and the last lines
                # are the panic's replay of it: it is shown whole instead.
                sections = [("FAILED lines", self.failed[:10]),
                            ("lockup report", self.lockup_report)]
            for title, lines in sections:
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
    parser.add_argument("--no-tests", action="store_true", help="skip the host tests")
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

    # The host tests run beside the boots: they take seconds, a boot tens of
    # them, so the check takes no longer for running both.
    tests = None if args.no_tests else HostTests(root, out, logdir)
    if tests is not None:
        tests.start()
    for b in boots:
        b.start()
    pending = list(boots)
    while pending:
        pending = [b for b in pending if not b.poll()]
        time.sleep(0.2)
    for b in boots:
        b.finish()
    if tests is not None:
        tests.finish()

    results = [b.report() for b in boots]
    if tests is not None:
        results.append(tests.report())
    print(f"logs       {logdir}/{{{','.join(b.tag for b in boots)}}}.log")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
