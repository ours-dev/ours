#!/usr/bin/env python3
"""Which tests run on the build machine: the one rule the scripts that run them share.

The host tests are the executables GN marks testonly, in the host toolchain, that
//:default builds. testonly is GN's own mark for a test, in the sense that only a
testonly target may depend on one; it is not a label that says "test", so
benchmarks carry it too, and //benches is left out, since a benchmark reports
timings and does not pass or fail.

The list is asked of GN each time and kept nowhere, so a test is found as soon as
it is in //:default: the testonly executables of the host toolchain (`gn ls`),
those of them the default build reaches (`gn desc //:default deps --all`), and
the file each one links (`gn outputs`).

Used by the `ours-verify` skill's boot_check.py and by scripts/cov_host.py.
"""
from __future__ import annotations

import subprocess
from pathlib import Path

HOST_TOOLCHAIN = "//build/toolchain/ours:host"
# Benchmarks are testonly too, but report timings and do not pass or fail.
NOT_TESTS = ("//benches/",)


class HostTestsError(RuntimeError):
    """GN could not say which the host tests are."""


def gn(root: Path, *args: str) -> list[str]:
    result = subprocess.run([str(root / "prebuilt/gn/gn"), *args], cwd=root,
                            capture_output=True, text=True)
    if result.returncode != 0:
        raise HostTestsError(f"gn {args[0]}: {(result.stdout + result.stderr).strip()[-500:]}")
    return [line.strip() for line in result.stdout.splitlines() if line.strip()]


def find(root: Path, out: Path) -> list[tuple[str, str]]:
    """(label, file relative to `out`) of every host test, in label order; empty
    if //:default builds none. `out` is a GN build directory that has been
    generated."""
    suffix = f"({HOST_TOOLCHAIN})"
    tests = {label for label in gn(root, "ls", str(out), "--testonly=true", "--type=executable")
             if label.endswith(suffix) and not label.startswith(NOT_TESTS)}
    built = set(gn(root, "desc", str(out), "//:default", "deps", "--all"))
    labels = sorted(tests & built)
    if not labels:
        return []
    outputs = gn(root, "outputs", str(out), *labels)
    if len(outputs) != len(labels):
        raise HostTestsError(f"gn outputs gave {len(outputs)} files for {len(labels)} tests")
    return list(zip(labels, outputs))
