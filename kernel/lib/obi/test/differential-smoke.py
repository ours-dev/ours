#!/usr/bin/env python3
"""differential-smoke — runs a batch of .rsp scenarios through the real
`obit` binary and asserts each one packs and validates cleanly, plus an
error-handling check (obit must reject a duplicate item name).

(Named "differential" from when this compared obit against a parallel
Python reimplementation of the packer; that reimplementation was removed —
obit is the single source of truth for packing, so this now just broadens
obit's own scenario coverage instead of diffing two implementations.)
"""
from __future__ import annotations

import argparse
import pathlib
import subprocess
import tempfile


def run_obit(obit: pathlib.Path, args: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run([str(obit), *args], capture_output=True, text=True)


def make_manifest(directory: pathlib.Path, name: str, entries: dict[str, bytes]) -> pathlib.Path:
    lines = []
    for dest, content in entries.items():
        src = directory / f"{name}-{dest.replace('/', '_')}.bin"
        src.write_bytes(content)
        lines.append(f"{dest}={src}")
    manifest = directory / f"{name}.fini"
    manifest.write_text("\n".join(lines) + "\n")
    return manifest


def scenario_rsp(directory: pathlib.Path, index: int, *, compressed: bool,
                  prefix: str, cmdline: list[str], embed_container: bool,
                  file_count: int) -> pathlib.Path:
    entries = {f"file{i}": f"payload for file {i} in scenario {index}".encode() * (i + 1)
               for i in range(file_count)}
    manifest = make_manifest(directory, f"scenario{index}", entries)

    lines = ["--reset", "--files-type=kernel-storage"]
    if prefix:
        lines.append(f"--prefix={prefix}")
    if compressed:
        lines.append("--compressed=lz4")
    lines += ["--files", str(manifest)]

    if cmdline:
        lines.append("--reset")
        lines.append("--type=cmdline")
        lines += [f"--entry={token}" for token in cmdline]

    if embed_container:
        raw = directory / f"scenario{index}-raw.bin"
        raw.write_bytes(f"raw embed payload {index}".encode())
        lines.append("--reset")
        lines.append("--type=container")
        lines.append(str(raw))

    rsp = directory / f"scenario{index}.rsp"
    rsp.write_text("\n".join(lines) + "\n")
    return rsp


def assert_scenario_packs_and_validates(obit: pathlib.Path, rsp: pathlib.Path,
                                         directory: pathlib.Path, index: int) -> None:
    out = directory / f"scenario{index}.obi"
    packed = run_obit(obit, ["pack", f"@{rsp}", "--output", str(out)])
    assert packed.returncode == 0, f"scenario {index}: obit pack failed: {packed.stderr}"

    validated = run_obit(obit, ["validate", str(out)])
    assert validated.returncode == 0, (
        f"scenario {index}: obit rejected its own output: {validated.stderr}"
    )


def assert_rejects_duplicate_item_name(obit: pathlib.Path, directory: pathlib.Path) -> None:
    first = directory / "dup-first.obi"
    (directory / "dup1.rsp").write_text("--reset\n--type=cmdline\n--entry=first=1\n")
    r = run_obit(obit, ["pack", f"@{directory / 'dup1.rsp'}", "--output", str(first)])
    assert r.returncode == 0, r.stderr

    dup_rsp = directory / "dup2.rsp"
    dup_rsp.write_text(
        "--reset\n--type=container\n"
        f"{first}\n"
        "--reset\n--type=container\n"
        f"{first}\n"
    )
    result = run_obit(obit, ["pack", f"@{dup_rsp}", "--output", str(directory / "dup-out.obi")])
    assert result.returncode != 0, "obit should reject a duplicate item name but did not"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stamp", type=pathlib.Path, required=True)
    parser.add_argument("--obit", type=pathlib.Path, required=True)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        directory = pathlib.Path(tmp)

        scenarios = [
            dict(compressed=True, prefix="ours", cmdline=["console=uart"], embed_container=True, file_count=2),
            dict(compressed=False, prefix="ours", cmdline=["console=uart"], embed_container=True, file_count=2),
            dict(compressed=True, prefix="", cmdline=[], embed_container=False, file_count=1),
            dict(compressed=True, prefix="a/b/c", cmdline=["x=1", "y=2", "z=3"], embed_container=False, file_count=3),
            dict(compressed=False, prefix="pkg", cmdline=[], embed_container=True, file_count=1),
            dict(compressed=True, prefix="pkg", cmdline=["solo=1"], embed_container=False, file_count=8),
        ]

        for index, scenario in enumerate(scenarios):
            rsp = scenario_rsp(directory, index, **scenario)
            assert_scenario_packs_and_validates(args.obit, rsp, directory, index)

        assert_rejects_duplicate_item_name(args.obit, directory)

    args.stamp.write_text("ok\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
