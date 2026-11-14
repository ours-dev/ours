#!/usr/bin/env python3
"""Boot the instrumented kernel, run its tests, and report which kernel code they reach.

    python3 scripts/cov_kernel.py [--out out/cov-kernel] [--html] [--no-tests]
                                  [--command CMD ...] [--no-build]

It works in a build directory of its own, configured with the kernel coverage
variant (`tools/set.py bringup.x86 --out out/cov-kernel --variant
coverage-kernel`; it runs that itself for a directory that does not exist yet).
In that directory the dbg image's kernel is built with source-based coverage
instrumentation and the rel image's is not.

What it does, in order:

  1. Builds the directory.
  2. Boots the dbg image under QEMU, as scripts/kshell_run.py does, and waits
     for the boot to end. Everything the boot itself ran is already counted.
  3. Types the kernel's tests, which nothing runs at boot. The test
     interface's tests are asked from the booted kernel with `ktest list` and
     run one at a time; the `*.test` console commands that have not migrated
     are taken from the GKTL_COMMAND registrations in kernel/commands. Both
     then run with any --command given. A test that fails is reported; the
     coverage of what it ran still counts.
  4. Types `cov verb=dump`. The kernel prints its counters as a raw profile in text
     lines (kernel/commands/cov.cpp), which are put back together here. Every
     line carries its own offset and the last carries a checksum, because the
     console is shared with the other CPUs' log lines; a dump that does not add
     up is asked for again.
  5. Merges the profile with llvm-profdata, using the unstripped kernel ELF,
     which holds the function records and names the profile leaves out, and
     asks llvm-cov for a per-file summary of it.
  6. Prints coverage per GN unit and in total, as scripts/cov_host.py does for
     the host tests, and writes the same into <out>/coverage/.

The numbers are the coverage of the code the dbg image's kernel was built from,
all of it, because the counters of code that never ran are in the profile as
zeros. They say nothing of the rel image.

llvm-cov and llvm-profdata come from scripts/fetch_llvm_coverage.py.
"""
from __future__ import annotations

import argparse
import base64
import binascii
import json
import re
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))

import cov_host  # noqa: E402
import fetch_llvm_coverage as tools  # noqa: E402
import kshell_run  # noqa: E402
from boot_logdir import log_directory  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent

VARIANT = "coverage-kernel"
KERNEL_ELF = f"kernel_x86.dbg.1-{VARIANT}/obj/kernel/products/bin/vmours.with-tests"

BOOT_TIMEOUT = 90       # seconds for the boot to end
COMMAND_TIMEOUT = 120   # seconds for one kernel test
DUMP_TIMEOUT = 30       # seconds for one `cov verb=dump`; a healthy one takes a few
DUMP_TRIES = 3

# The tags of kernel/commands/cov.cpp's lines, and the checksum it ends with.
BEGIN_LINE = re.compile(r"COV-BEGIN (\d+) (\d+)\s*$")
DATA_LINE = re.compile(r"(?:^|\s)COV (\d+) ([A-Za-z0-9+/]+={0,2})\s*$")
END_LINE = re.compile(r"COV-END (\d+) ([0-9a-f]{16})\s*$")

FNV_OFFSET_BASIS = 0xcbf29ce484222325
FNV_PRIME = 0x100000001b3
MASK_64 = (1 << 64) - 1

# One line of the test interface's `ktest list` (kernel/lib/ktest/src/runner.cpp).
KTEST_TEST = re.compile(r"ktest: TEST (\S+)")


class KernelCovError(Exception):
    """The kernel coverage run could not be set up or completed."""


def fnv1a64(data: bytes) -> int:
    """The checksum kernel/commands/cov.cpp ends its dump with."""
    value = FNV_OFFSET_BASIS
    for byte in data:
        value = ((value ^ byte) * FNV_PRIME) & MASK_64
    return value


def parse_dump(text: str) -> Optional[bytes]:
    """The raw profile a `cov verb=dump` printed in `text`, or None if there is no
    dump in it that adds up. The last complete one wins: text may hold several.

    A line is one of `COV-BEGIN <size> <chunk>`, `COV <offset> <base64>` and
    `COV-END <size> <checksum>`. Text before the tag on a line is allowed,
    because the prompt and the echo of the typed command end no line and the
    first output follows them; text after it is not, and neither is anything
    but base64 in the middle, so a line another writer broke is dropped, and
    the offsets and the checksum say that it was.
    """
    found: Optional[bytes] = None
    size = chunk = -1
    pieces: dict[int, bytes] = {}
    for raw in text.splitlines():
        line = raw.rstrip("\r")
        begin = BEGIN_LINE.search(line)
        if begin:
            size, chunk = int(begin.group(1)), int(begin.group(2))
            pieces = {}
            continue
        if size < 0:
            continue
        data = DATA_LINE.search(line)
        if data:
            try:
                decoded = base64.b64decode(data.group(2), validate=True)
            except (binascii.Error, ValueError):
                continue
            pieces.setdefault(int(data.group(1)), decoded)
            continue
        end = END_LINE.search(line)
        if end:
            candidate = assemble(size, chunk, pieces)
            if (candidate is not None and int(end.group(1)) == size
                    and fnv1a64(candidate) == int(end.group(2), 16)):
                found = candidate
            size = -1
    return found


def assemble(size: int, chunk: int, pieces: dict[int, bytes]) -> Optional[bytes]:
    """The profile from its lines, if there is exactly the line that should
    be at each offset and no other."""
    if chunk <= 0 or size < 0:
        return None
    expected = list(range(0, size, chunk))
    if sorted(pieces) != expected:
        return None
    out = bytearray()
    for offset in expected:
        if len(pieces[offset]) != min(chunk, size - offset):
            return None
        out += pieces[offset]
    return bytes(out)


def ktest_commands(text: str) -> list[str]:
    """The commands that run the tests a `ktest list` printed, one each."""
    return [f"ktest {name}" for name in KTEST_TEST.findall(text)]


def ensure_configured(root: Path, out: Path) -> None:
    """The build directory must select the kernel coverage variant; a directory
    that does not exist is configured, one configured without it is refused."""
    if not (out / "build.ninja").is_file():
        print(f"configure  {out} is not configured; running tools/set.py")
        result = subprocess.run(
            [sys.executable, "tools/set.py", "bringup.x86", "--out", str(out), "--variant", VARIANT],
            cwd=root, capture_output=True, text=True)
        if result.returncode != 0:
            raise KernelCovError("configure failed:\n" + (result.stdout + result.stderr)[-2000:])
        return
    args_gn = out / "args.gn"
    text = args_gn.read_text() if args_gn.is_file() else ""
    if not re.search(r'select_variant\s*=\s*\[\s*"' + VARIANT + r'"\s*\]', text):
        raise KernelCovError(
            f'{out} is not configured for kernel coverage: its args.gn does not say '
            f'select_variant = ["{VARIANT}"]. Use another --out.')


def build(root: Path, out: Path) -> float:
    start = time.monotonic()
    result = subprocess.run([str(root / "prebuilt/ninja/ninja"), "-C", str(out)], cwd=root,
                            capture_output=True, text=True)
    if result.returncode != 0:
        lines = [l for l in (result.stdout + result.stderr).splitlines()
                 if "-lto_library" not in l]
        raise KernelCovError("build failed:\n" + "\n".join(lines[-40:]))
    return time.monotonic() - start


def kernel_elf(out: Path) -> Path:
    """The unstripped kernel of the dbg image: the one with the function records
    and names that the profile leaves out."""
    elf = out / KERNEL_ELF
    if not elf.is_file():
        raise KernelCovError(f"{elf} is missing: is {out} built for {VARIANT}?")
    return elf


class Boot:
    """What one boot of the dbg image came to."""

    def __init__(self) -> None:
        # The last complete dump. Counters only grow, so it holds everything
        # the machine did before it.
        self.profile: Optional[bytes] = None
        self.failed: list[str] = []   # commands that ran and did not pass
        self.killer: Optional[str] = None  # the command after which the machine was gone
        self.ran = 0                  # commands that finished, the killer not among them


def type_command(console: kshell_run.Console, command: str) -> bool:
    """Types `command` and waits for it to end; whether it passed."""
    name = command.split()[0]
    ktest = kshell_run.is_ktest(command)
    console.keep()
    console.type(command)
    if name.endswith(".test"):
        verdict = re.compile(re.escape(name) + r": (ok|FAILED)\b[^\n]*\n")
    elif ktest:
        verdict = kshell_run.KTEST_END
    else:
        verdict = re.compile(re.escape(kshell_run.PROMPT))
    ended = console.wait_for(verdict, time.monotonic() + COMMAND_TIMEOUT, through=True)
    text = console.kept()
    console.drain(kshell_run.AFTER_VERDICT)
    if name.endswith(".test"):
        return ended and f"{name}: ok" in text
    if ktest:
        summaries = kshell_run.ktest_summaries(text.splitlines())
        return ended and len(summaries) == 1 and summaries[0]["failed"] == 0
    return ended


def discover_ktest_commands(out: Path, logdir: Path) -> list[str]:
    """The tests the kernel's test interface registered, asked from a boot of
    its own. A kernel without the interface answers nothing, and the list is
    empty; that is not an error, the `*.test` commands still cover it."""
    console = kshell_run.Console(out, "dbg", logdir, kshell_run.SILENCE)
    try:
        if not console.wait_for(re.compile(re.escape(kshell_run.END_MARKER)),
                                time.monotonic() + BOOT_TIMEOUT):
            raise KernelCovError(
                f'no "{kshell_run.END_MARKER}" within {BOOT_TIMEOUT}s of booting; see {console.log_path}')
        console.drain(kshell_run.SETTLE)
        console.keep()
        console.type("ktest list")
        if not console.wait_for(re.compile(re.escape(kshell_run.PROMPT)),
                                time.monotonic() + COMMAND_TIMEOUT):
            raise KernelCovError(f"`ktest list` did not return to the prompt within {COMMAND_TIMEOUT}s")
        return ktest_commands(console.kept())
    finally:
        console.close()


def dump(console: kshell_run.Console) -> Optional[bytes]:
    """The kernel's counters. Asked again, up to DUMP_TRIES times, when lines
    came but did not add up, which the console being shared can do. Not asked
    again when nothing came at all within DUMP_TIMEOUT, for a healthy dump takes
    a couple of seconds: that machine is not answering, and None says so."""
    for attempt in range(1, DUMP_TRIES + 1):
        if console.gone or console.panicked:
            return None
        console.keep()
        console.type("cov verb=dump")
        end = re.compile(r"COV-END [^\n]*\n|cov: [^\n]*\n")
        if not console.wait_for(end, time.monotonic() + DUMP_TIMEOUT, through=True):
            print(f"  dump attempt {attempt}: no end line within {DUMP_TIMEOUT}s", flush=True)
            return None
        text = console.kept()
        if "this kernel was built without coverage" in text:
            raise KernelCovError("this kernel was built without coverage instrumentation")
        profile = parse_dump(text)
        if profile is not None:
            return profile
        print(f"  dump attempt {attempt}: lines missing or damaged, asking again", flush=True)
    return None


def run_boot(out: Path, commands: list[str], logdir: Path) -> Boot:
    """Boots the dbg image and runs `commands`, dumping the counters after each,
    until the machine is gone or the commands are done. With none, dumps once:
    what the boot itself ran."""
    boot = Boot()
    console = kshell_run.Console(out, "dbg", logdir, kshell_run.SILENCE)
    try:
        if not console.wait_for(re.compile(re.escape(kshell_run.END_MARKER)),
                                time.monotonic() + BOOT_TIMEOUT):
            raise KernelCovError(
                f'no "{kshell_run.END_MARKER}" within {BOOT_TIMEOUT}s of booting; see {console.log_path}')
        console.drain(kshell_run.SETTLE)

        if not commands:
            boot.profile = dump(console)
        for command in commands:
            try:
                passed = type_command(console, command)
                after = dump(console)
            except OSError:
                # QEMU went away under the typing: nothing more can be asked.
                passed, after = False, None
            if after is not None:
                boot.profile = after
            if after is None and (console.gone or console.panicked or not passed):
                boot.killer = command
                print(f"  GONE at {command}: the machine is not there", flush=True)
                break
            boot.ran += 1
            print(f"  {'ok  ' if passed else 'FAIL'} {command}", flush=True)
            if not passed:
                boot.failed.append(command)
        return boot
    finally:
        console.close()
        print(f"log        {console.log_path}")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, default=Path("out/cov-kernel"),
                        help="the coverage build directory (default: out/cov-kernel)")
    parser.add_argument("--no-build", action="store_true", help="use the directory as it is built")
    parser.add_argument("--no-tests", action="store_true",
                        help="run no test at all, neither the interface's nor a *.test command: "
                             "the coverage is the boot's alone")
    parser.add_argument("--command", action="append", default=[], metavar="CMD",
                        help="another kernel-shell command to type before the dump")
    parser.add_argument("--html", action="store_true",
                        help="also write an annotated HTML report to <out>/coverage/kernel-html")
    parser.add_argument("--all", action="store_true",
                        help="count test sources too (vendored code and the toolchain stay out)")
    args = parser.parse_args(argv)
    out = args.out if args.out.is_absolute() else ROOT / args.out

    def say(message: str) -> None:
        print(message, flush=True)

    try:
        compiler = tools.find_compiler(None)
        llvm_cov = tools.locate_tool(compiler, "llvm-cov")
        llvm_profdata = tools.locate_tool(compiler, "llvm-profdata")
        if llvm_cov is None or llvm_profdata is None:
            raise KernelCovError("llvm-cov and llvm-profdata are not available for "
                                 f"{compiler.clang}; run python3 scripts/fetch_llvm_coverage.py")

        ensure_configured(ROOT, out)
        if not args.no_build:
            say(f"build      {build(ROOT, out):.1f}s")
        elf = kernel_elf(out)

        logdir = log_directory("ours-covkernel", out.resolve())
        logdir.mkdir(parents=True, exist_ok=True)
        discovered = [] if args.no_tests else discover_ktest_commands(out, logdir)
        commands = discovered
        commands += args.command
        say(f"boot       dbg image, then {len(commands)} command(s)")

        # A command that takes the machine down (a lockup, a panic) ends that
        # boot. What it ran before is in the last dump; the commands after it
        # run in a fresh boot, and the profiles of the boots are summed.
        #
        # The machine may be found gone at a command that never ran: the one
        # before it passed, and the lockup came after. So the command it is
        # found gone at runs again first in the next boot, and only if a fresh
        # machine does not survive it as the very first command is it the
        # killer, and left out.
        profiles: list[bytes] = []
        failed: list[str] = []
        killers: list[str] = []
        remaining = commands
        while True:
            boot = run_boot(out, remaining, logdir)
            if boot.profile is not None:
                profiles.append(boot.profile)
            failed += boot.failed
            if boot.killer is None:
                break
            skip = 0
            if boot.ran == 0:
                killers.append(boot.killer)
                skip = 1
            remaining = remaining[boot.ran + skip:]
            if not remaining:
                break
        if not profiles:
            raise KernelCovError("no complete profile came out of the boots; see the logs above")

        directory = out / "coverage"
        directory.mkdir(parents=True, exist_ok=True)
        raws = []
        for index, profile in enumerate(profiles):
            raw = directory / f"kernel-{index}.profraw"
            raw.write_bytes(profile)
            raws.append(raw)
        merged = directory / "kernel.profdata"
        result = subprocess.run([str(llvm_profdata), "merge", "-sparse", f"--binary-file={elf}",
                                 *map(str, raws), "-o", str(merged)], capture_output=True, text=True)
        if result.returncode != 0:
            raise KernelCovError("llvm-profdata merge failed:\n" + result.stderr[-2000:])
        say(f"profile    {len(profiles)} boot(s), {sum(map(len, profiles))} bytes, "
            f"merged against {elf.relative_to(out)}")

        export = cov_host.export_summary(llvm_cov, out, [elf], merged, args.all)
        (directory / "kernel-summary.json").write_text(json.dumps(export))
        units, total = cov_host.summarize(ROOT, out, export)
        text = cov_host.render(units, total)
        (directory / "kernel-report.txt").write_text(text + "\n")
        say("")
        say(text)
        say("")
        say(f"written    {directory}/{{kernel-N.profraw,kernel.profdata,kernel-report.txt,kernel-summary.json}}")
        if args.html:
            cov_host.write_html(llvm_cov, out, [elf], merged, directory / "kernel-html", args.all)
            say(f"html       {directory}/kernel-html/index.html")
        if killers:
            say(f"killed the machine, on a fresh boot: {', '.join(killers)} (their own coverage is lost; "
                f"what ran before is counted)")
        if failed:
            say(f"failed     {', '.join(failed)}")
        return 1 if failed or killers else 0
    except (KernelCovError, cov_host.CovError, tools.FetchError, subprocess.SubprocessError,
            OSError) as error:
        print(f"cov_kernel.py: error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
