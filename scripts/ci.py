#!/usr/bin/env python3
"""What CI runs, as one script that runs the same way on any checkout.

    python3 scripts/ci.py check --base REV
    python3 scripts/ci.py verify [--out DIR] [--timeout SECONDS]
    python3 scripts/ci.py artifacts [--out DIR] [--dest DIR]
    python3 scripts/ci.py all --base REV [--out DIR] [--timeout SECONDS] [--dest DIR]

The workflow under .github/workflows only checks out, restores caches and calls
this, so a red run is reproduced by running the same command locally.

check: the pre-commit checks, over everything since REV instead of over the
index: C/C++ lines clang-format clean (format_cpp.py), GN files in gn's layout
(format_gn.py), documents in the layout standard (lint_docs.py), then the
scripts' own unit tests. "Since REV" is from the merge base of REV and HEAD, so
a branch is judged on its own commits, not on what its base gained since. The
checkers compare against HEAD by themselves; here OURS_DIFF_BASE points them at
that merge base instead (scripts/gitdiff.py). A REV of all zeros, which GitHub
sends for the first push of a branch, means origin/main. Unlike the hook, a
checker that cannot run fails the check: on a CI runner that is a broken
setup, not a missing tool on somebody's laptop.

verify: the verify skill's boot_check.py on --out (default out/ci.x86): build
bringup.x86, run the host tests, boot both images. The timeout is three times
the skill's, for a runner slower than a workstation.

artifacts: copies into --dest (default out/ci-artifacts) the images named by
the build's product_info.json, their .debug.json lists and the unstripped ELFs
those name, and the boot and host-test logs, then writes SHA256SUMS. What it
copies comes from GN's metadata, so a new image needs no edit here. A file that
is missing, after a failed build, is reported and skipped.

all: check, verify and artifacts, in that order. Every one runs whatever the
ones before it found, so one run shows a format slip and a failing boot
together; the exit status is 1 if any failed.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Optional

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))

from boot_logdir import log_directory  # noqa: E402
from gitdiff import BASE_VARIABLE, git_env  # noqa: E402

DEFAULT_OUT = "out/ci.x86"
DEFAULT_DEST = "out/ci-artifacts"
PRODUCT = "bringup.x86"
# boot_check.py's default is 60 s a boot; a CI runner has a third of the cores.
DEFAULT_TIMEOUT = 180
# What GitHub sends as the "before" commit of a branch's first push.
NO_COMMIT = "0" * 40
FIRST_PUSH_BASE = "origin/main"

SOURCE_SUFFIXES = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inc")
GN_SUFFIXES = (".gn", ".gni")


class CiError(Exception):
    """The run could not be set up: an unknown base, an unreadable build."""


def git(*args: str, cwd: Path = ROOT) -> str:
    result = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True,
                            env=git_env())
    if result.returncode != 0:
        raise CiError(f"git {' '.join(args)}: {result.stderr.strip()}")
    return result.stdout


def merge_base(base: str, cwd: Path = ROOT) -> str:
    if not base or base == NO_COMMIT:
        base = FIRST_PUSH_BASE
    return git("merge-base", base, "HEAD", cwd=cwd).strip()


def changed_paths(base: str, cwd: Path = ROOT) -> list[str]:
    """Files added, copied, modified or renamed from `base` to HEAD."""
    out = git("diff", "--name-only", "-z", "--diff-filter=ACMR", base, "HEAD", cwd=cwd)
    return sorted(path for path in out.split("\0") if path)


def report(name: str, ok: bool, detail: str = "") -> bool:
    print(f"{name:<14} {'PASS' if ok else 'FAIL'}{'  ' + detail if detail else ''}", flush=True)
    return ok


def run_step(name: str, command: list[str], env: Optional[dict] = None) -> bool:
    print(f"--- {name}: {' '.join(command)}", flush=True)
    result = subprocess.run(command, cwd=ROOT, env=env)
    return report(name, result.returncode == 0,
                  "" if result.returncode == 0 else f"exit {result.returncode}")


def check(base: str) -> bool:
    try:
        start = merge_base(base)
        paths = changed_paths(start)
    except CiError as error:
        return report("check", False, str(error))
    print(f"--- check: {len(paths)} file(s) changed since {start[:12]}", flush=True)
    env = {**os.environ, BASE_VARIABLE: start}
    python = sys.executable
    results = []
    groups = (("format-cpp", "format_cpp.py", ["--check"], SOURCE_SUFFIXES),
              ("format-gn", "format_gn.py", ["--check"], GN_SUFFIXES),
              ("lint-docs", "lint_docs.py", ["--changed"], (".md",)))
    for name, script, flags, suffixes in groups:
        chosen = [path for path in paths if path.endswith(suffixes)]
        if not chosen:
            results.append(report(name, True, "nothing changed"))
            continue
        results.append(run_step(name, [python, f"scripts/{script}", *flags, *chosen], env))
    results.append(run_step("script-tests", [python, "-m", "unittest", "discover", "-s",
                                             "scripts", "-p", "test_*.py"]))
    return all(results)


def verify(out: str, timeout: float) -> bool:
    return run_step("verify", [sys.executable, ".agents/skills/verify/scripts/boot_check.py",
                               "--out", out, "--product", PRODUCT,
                               "--timeout", str(timeout)])


def copy(source: Path, target: Path, missing: list[str]) -> None:
    if not source.is_file():
        missing.append(str(source))
        return
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)


def collect(out: Path, dest: Path) -> list[str]:
    """Fills `dest`; returns what it could not find."""
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    missing: list[str] = []
    info = out / "product_info.json"
    products = json.loads(info.read_text(encoding="utf-8")) if info.is_file() else []
    if not products:
        missing.append(str(info))
    for product in products:
        for image in product.get("kernel_images", []):
            name = image["output_name"]
            copy(out / f"kernel.{name}.obi", dest / f"kernel.{name}.obi", missing)
            listing = out / f"kernel.{name}.debug.json"
            copy(listing, dest / listing.name, missing)
            if not listing.is_file():
                continue
            # One record names the image itself ("obi", "image", "label"); every
            # other names an unstripped ELF that went into it.
            for entry in json.loads(listing.read_text(encoding="utf-8")):
                if "debug" in entry:
                    copy(out / entry["debug"], dest / "debug" / entry["debug"], missing)
    logs = log_directory("ours-verify", out.resolve())
    for log in sorted(logs.glob("*.log")) if logs.is_dir() else []:
        copy(log, dest / "logs" / log.name, missing)
    sums = []
    for path in sorted(p for p in dest.rglob("*") if p.is_file()):
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        sums.append(f"{digest}  {path.relative_to(dest).as_posix()}\n")
    (dest / "SHA256SUMS").write_text("".join(sums), encoding="utf-8")
    return missing


def artifacts(out: str, dest: str) -> bool:
    try:
        missing = collect(ROOT / out, ROOT / dest)
    except (OSError, ValueError, KeyError) as error:
        return report("artifacts", False, str(error))
    for path in missing:
        print(f"artifacts: missing {path}", flush=True)
    files = sum(1 for p in (ROOT / dest).rglob("*") if p.is_file())
    return report("artifacts", not missing, f"{files} file(s) in {dest}")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("check", "verify", "artifacts", "all"):
        command = sub.add_parser(name)
        if name in ("check", "all"):
            command.add_argument("--base", required=True,
                                 help="the revision the push or pull request starts from")
        if name in ("verify", "artifacts", "all"):
            command.add_argument("--out", default=DEFAULT_OUT, help=f"build directory (default: {DEFAULT_OUT})")
        if name in ("verify", "all"):
            command.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT,
                                 help=f"seconds per boot (default: {DEFAULT_TIMEOUT})")
        if name in ("artifacts", "all"):
            command.add_argument("--dest", default=DEFAULT_DEST, help=f"where artifacts go (default: {DEFAULT_DEST})")
    args = parser.parse_args(argv)

    results = []
    if args.command in ("check", "all"):
        results.append(check(args.base))
    if args.command in ("verify", "all"):
        results.append(verify(args.out, args.timeout))
    if args.command in ("artifacts", "all"):
        results.append(artifacts(args.out, args.dest))
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
