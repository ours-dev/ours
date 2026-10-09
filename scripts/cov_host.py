#!/usr/bin/env python3
"""Run the host tests with coverage instrumentation and report which code they reach.

    python3 scripts/cov_host.py [--out out/cov-host] [--html] [--all] [--jobs N]

It works in a build directory of its own, configured with the host toolchain's
coverage variant (`tools/set.py bringup.x86 --out out/cov-host --args
'select_variant=[{variant="coverage" host=true}]'`; it runs that itself for a
directory that does not exist yet). Everything the host toolchain builds there
is instrumented, so it is not the directory to build images in.

What it does, in order:

  1. Asks GN which host tests there are: the testonly executables of the host
     toolchain that //:default builds, the same rule the `ours-verify` skill
     uses for its host tests, so a test added to //:default is covered from its
     first day and there is no second list to keep.
  2. Builds them all.
  3. Runs each from the build directory with its own raw profile file; a test
     that fails is reported, and its profile is kept when it wrote one.
  4. Merges the profiles with llvm-profdata and asks llvm-cov for a per-file
     summary of every binary together.
  5. Prints coverage per GN unit (the nearest directory above a source file
     that holds a BUILD.gn) and in total, and writes the same into
     <out>/coverage/. Test sources, vendored code, the toolchain and generated
     files are left out of the numbers unless --all is given: what is asked is
     which code the tests reach, not how much of the tests ran.

llvm-cov and llvm-profdata come from scripts/fetch_llvm_coverage.py, which this
does not run itself because it downloads over a gigabyte.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))

import fetch_llvm_coverage as tools  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent

HOST_TOOLCHAIN = "//build/toolchain/ours:host"
# Benchmarks are testonly too, but report timings and do not pass or fail.
NOT_TESTS = ("//benches/",)
TEST_TIMEOUT = 300  # seconds for one test binary

SELECTOR_VARIANT = re.compile(r'variant\s*=\s*"coverage"')
SELECTOR_HOST = re.compile(r"host\s*=\s*true")

# Directories whose sources are not what the tests are asked about.
NEVER_COUNTED = ("vendor", "prebuilt", "benches")
TEST_DIRECTORIES = ("test", "tests")


class CovError(Exception):
    """The coverage run could not be set up or completed."""


def gn(root: Path, *args: str) -> list[str]:
    result = subprocess.run([str(root / "prebuilt/gn/gn"), *args], cwd=root,
                            capture_output=True, text=True)
    if result.returncode != 0:
        raise CovError(f"gn {args[0]}: {(result.stdout + result.stderr).strip()[-500:]}")
    return [line.strip() for line in result.stdout.splitlines() if line.strip()]


def ensure_configured(root: Path, out: Path) -> None:
    """The build directory must carry the host coverage selector; a directory
    that is missing is configured, one that is configured without it is refused
    rather than reconfigured behind the user's back."""
    args_gn = out / "args.gn"
    if not (out / "build.ninja").is_file():
        print(f"configure  {out} is not configured; running tools/set.py")
        result = subprocess.run(
            [sys.executable, "tools/set.py", "bringup.x86", "--out", str(out), "--args",
             'select_variant=[{variant="coverage" host=true}]'],
            cwd=root, capture_output=True, text=True)
        if result.returncode != 0:
            raise CovError("configure failed:\n" + (result.stdout + result.stderr)[-2000:])
        return
    text = args_gn.read_text() if args_gn.is_file() else ""
    if not (SELECTOR_VARIANT.search(text) and SELECTOR_HOST.search(text)):
        raise CovError(
            f"{out} is not configured for host coverage: its args.gn has no "
            f'select_variant selector {{variant="coverage" host=true}}. Use another '
            f"--out, or add the selector with `gn args {out}`.")


def host_tests(root: Path, out: Path) -> list[tuple[str, str]]:
    """(label, output path relative to `out`) of every host test, from GN. The
    same rule as HostTests in .agents/skills/ours-verify/scripts/boot_check.py."""
    suffix = f"({HOST_TOOLCHAIN})"
    tests = {label for label in gn(root, "ls", str(out), "--testonly=true", "--type=executable")
             if label.endswith(suffix) and not label.startswith(NOT_TESTS)}
    built = set(gn(root, "desc", str(out), "//:default", "deps", "--all"))
    labels = sorted(tests & built)
    if not labels:
        raise CovError("GN found no host tests in //:default")
    outputs = gn(root, "outputs", str(out), *labels)
    if len(outputs) != len(labels):
        raise CovError(f"gn outputs gave {len(outputs)} files for {len(labels)} tests")
    return list(zip(labels, outputs))


def build(root: Path, out: Path, outputs: list[str]) -> float:
    start = time.monotonic()
    result = subprocess.run([str(root / "prebuilt/ninja/ninja"), "-C", str(out), *outputs],
                            cwd=root, capture_output=True, text=True)
    if result.returncode != 0:
        lines = [l for l in (result.stdout + result.stderr).splitlines()
                 if "-lto_library" not in l]
        raise CovError("build failed:\n" + "\n".join(lines[-40:]))
    return time.monotonic() - start


def run_tests(out: Path, tests: list[tuple[str, str]], profiles: Path,
              jobs: int) -> list[tuple[str, str]]:
    """Runs every test and returns (label, problem) for those that did not exit 0."""
    for stale in profiles.glob("*.profraw"):
        stale.unlink()  # a profile of an older build would not match these binaries
    profiles.mkdir(parents=True, exist_ok=True)

    def run_one(item: tuple[int, tuple[str, str]]) -> Optional[tuple[str, str]]:
        index, (label, output) = item
        env = dict(os.environ, LLVM_PROFILE_FILE=str(profiles / f"{index:03d}-%p.profraw"))
        try:
            result = subprocess.run([str(out / output)], cwd=out, env=env, capture_output=True,
                                    text=True, errors="replace", timeout=TEST_TIMEOUT)
        except subprocess.TimeoutExpired:
            return label, f"no exit within {TEST_TIMEOUT}s"
        except OSError as error:
            return label, f"cannot run: {error}"
        if result.returncode != 0:
            tail = (result.stdout + result.stderr).strip().splitlines()[-3:]
            return label, f"exit {result.returncode}: " + " | ".join(tail)
        return None

    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        results = list(pool.map(run_one, enumerate(tests)))
    return [r for r in results if r is not None]


def merge(llvm_profdata: Path, profiles: Path, merged: Path) -> int:
    files = sorted(profiles.glob("*.profraw"))
    if not files:
        raise CovError("no test wrote a profile; are the tests really instrumented?")
    listing = profiles / "files.txt"
    listing.write_text("\n".join(str(f) for f in files) + "\n")
    result = subprocess.run([str(llvm_profdata), "merge", "-sparse", "-f", str(listing),
                             "-o", str(merged)], capture_output=True, text=True)
    if result.returncode != 0:
        raise CovError("llvm-profdata merge failed:\n" + result.stderr[-2000:])
    return len(files)


def ignore_regex(include_all: bool) -> str:
    parts = list(NEVER_COUNTED)
    if not include_all:
        parts += list(TEST_DIRECTORIES)
    return r"(^|/)(" + "|".join(parts) + r")/|^/(Library|usr|opt)/|(^|/)out/"


def export_summary(llvm_cov: Path, out: Path, binaries: list[Path], merged: Path,
                   include_all: bool) -> dict:
    command = [str(llvm_cov), "export", str(binaries[0])]
    for binary in binaries[1:]:
        command += ["-object", str(binary)]
    command += [f"-instr-profile={merged}", "-summary-only",
                f"-ignore-filename-regex={ignore_regex(include_all)}"]
    result = subprocess.run(command, cwd=out, capture_output=True, text=True, timeout=1800)
    if result.returncode != 0:
        raise CovError("llvm-cov export failed:\n" + result.stderr[-2000:])
    return json.loads(result.stdout)


def unit_of(root: Path, relative: str, cache: dict[str, str]) -> str:
    """The nearest directory at or above the file that holds a BUILD.gn."""
    directory = os.path.dirname(relative)
    if directory in cache:
        return cache[directory]
    probe = directory
    while probe and not (root / probe / "BUILD.gn").is_file():
        probe = os.path.dirname(probe)
    cache[directory] = probe or "."
    return cache[directory]


def counted(root: Path, out: Path, filename: str) -> Optional[str]:
    """The path relative to `root` when the file is the repository's own source.
    llvm-cov names a file as the compiler recorded it, which may be relative to
    the build directory (`../../kernel/...`), so it is resolved against `out`."""
    path = Path(os.path.normpath(os.path.join(out, filename)))
    try:
        relative = path.relative_to(root)
    except ValueError:
        return None
    return None if relative.parts[0] == "out" else str(relative)


KINDS = ("lines", "functions", "regions", "branches")


def summarize(root: Path, out: Path,
              export: dict) -> tuple[dict[str, dict[str, list[int]]], dict[str, list[int]]]:
    """Per-unit and total [covered, count] for each kind, from llvm-cov's per-file
    summaries, counting only the repository's own sources."""
    units: dict[str, dict[str, list[int]]] = {}
    total = {kind: [0, 0] for kind in KINDS}
    cache: dict[str, str] = {}
    for file in export["data"][0]["files"]:
        relative = counted(root, out, file["filename"])
        if relative is None:
            continue
        unit = units.setdefault(unit_of(root, relative, cache),
                                {kind: [0, 0] for kind in KINDS})
        for kind in KINDS:
            covered, count = file["summary"][kind]["covered"], file["summary"][kind]["count"]
            unit[kind][0] += covered
            unit[kind][1] += count
            total[kind][0] += covered
            total[kind][1] += count
    return units, total


def percent(pair: list[int]) -> str:
    return "-" if pair[1] == 0 else f"{100.0 * pair[0] / pair[1]:5.1f}%"


def render(units: dict[str, dict[str, list[int]]], total: dict[str, list[int]]) -> str:
    width = max([len(u) for u in units] + [len("unit")])
    header = (f"{'unit':<{width}}  {'lines':>7}  {'regions':>8}  {'branches':>8}  "
              f"{'functions':>9}  {'lines hit/all':>13}")
    rows = [header, "-" * len(header)]
    for unit in sorted(units):
        d = units[unit]
        rows.append(f"{unit:<{width}}  {percent(d['lines']):>7}  {percent(d['regions']):>8}  "
                    f"{percent(d['branches']):>8}  {percent(d['functions']):>9}  "
                    f"{d['lines'][0]:>5}/{d['lines'][1]:<5}")
    rows.append("-" * len(header))
    rows.append(f"{'TOTAL':<{width}}  {percent(total['lines']):>7}  {percent(total['regions']):>8}  "
                f"{percent(total['branches']):>8}  {percent(total['functions']):>9}  "
                f"{total['lines'][0]:>5}/{total['lines'][1]:<5}")
    return "\n".join(rows)


def write_html(llvm_cov: Path, out: Path, binaries: list[Path], merged: Path,
               directory: Path, include_all: bool) -> None:
    command = [str(llvm_cov), "show", str(binaries[0])]
    for binary in binaries[1:]:
        command += ["-object", str(binary)]
    command += [f"-instr-profile={merged}", "-format=html", f"-output-dir={directory}",
                f"-ignore-filename-regex={ignore_regex(include_all)}"]
    result = subprocess.run(command, cwd=out, capture_output=True, text=True, timeout=1800)
    if result.returncode != 0:
        raise CovError("llvm-cov show failed:\n" + result.stderr[-2000:])


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, default=Path("out/cov-host"),
                        help="the coverage build directory (default: out/cov-host)")
    parser.add_argument("--html", action="store_true",
                        help="also write an annotated HTML report to <out>/coverage/html")
    parser.add_argument("--all", action="store_true",
                        help="count test sources too (vendored code and the toolchain stay out)")
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 4),
                        help="tests run at once")
    args = parser.parse_args(argv)
    out = args.out if args.out.is_absolute() else ROOT / args.out

    def say(message: str) -> None:
        print(message, flush=True)

    try:
        compiler = tools.find_compiler(None)
        llvm_cov = tools.locate_tool(compiler, "llvm-cov")
        llvm_profdata = tools.locate_tool(compiler, "llvm-profdata")
        if llvm_cov is None or llvm_profdata is None:
            raise CovError("llvm-cov and llvm-profdata are not available for "
                           f"{compiler.clang}; run python3 scripts/fetch_llvm_coverage.py")

        ensure_configured(ROOT, out)
        tests = host_tests(ROOT, out)
        say(f"tests      {len(tests)} host tests from GN")
        say(f"build      {build(ROOT, out, [output for _, output in tests]):.1f}s")

        directory = out / "coverage"
        started = time.monotonic()
        failed = run_tests(out, tests, directory / "profraw", args.jobs)
        say(f"run        {len(tests) - len(failed)} passed, {len(failed)} failed "
            f"· {time.monotonic() - started:.1f}s")
        for label, problem in failed[:10]:
            say(f"  {label}: {problem[:200]}")

        merged = directory / "all.profdata"
        say(f"merge      {merge(llvm_profdata, directory / 'profraw', merged)} profiles")
        binaries = [out / output for _, output in tests]
        export = export_summary(llvm_cov, out, binaries, merged, args.all)
        (directory / "summary.json").write_text(json.dumps(export))
        units, total = summarize(ROOT, out, export)
        text = render(units, total)
        (directory / "report.txt").write_text(text + "\n")
        say("")
        say(text)
        say("")
        say(f"written    {directory}/{{report.txt,summary.json,all.profdata}}")
        if args.html:
            write_html(llvm_cov, out, binaries, merged, directory / "html", args.all)
            say(f"html       {directory}/html/index.html")
        return 1 if failed else 0
    except (CovError, tools.FetchError, subprocess.SubprocessError, OSError) as error:
        print(f"cov_host.py: error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
