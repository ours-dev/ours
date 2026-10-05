#!/usr/bin/env python3
"""Configure a build directory for a product and target CPU.

Usage:
  tools/set.py <arch>                 # e.g. tools/set.py x86 (default product: bringup)
  tools/set.py <product>.<arch>       # e.g. tools/set.py bringup.x86
  tools/set.py <product>.<arch> --variant lto[,ubsan-trap]
  tools/set.py <product>.<arch> --clang-dir <path>  # override the clang tool dir
  tools/set.py <product>.<arch> --args kernel_shell_enabled=false  # any GN arg
  tools/set.py <product>.<arch> --out out/mine --with //examples/foo --autorun bin/foo+arg
  tools/set.py --list                 # list valid products and arches

It writes args.gn with the selected product (ours_product), target CPU, and
clang_tool_dir, then runs gn gen.  The build directory defaults to
out/<product>.<arch> (e.g. out/bringup.x86); --out overrides it.  Build
everything with:

  ninja -C out/<product>.<arch> //products/<product>:<product>

clang_tool_dir is resolved automatically (--clang-dir, then the
CLANG_TOOL_DIR environment variable, then prebuilt/clang/*/bin) and written
into args.gn.  The Ours kernel toolchains derive their libc++ include
directory from clang_tool_dir (see build/toolchain/ours/ours_toolchain.gni),
so a build generated without it cannot find the C++ standard library headers
(<cstdint>, <format>, ...) and the kernel compile fails at the first
include.  A missing clang tool dir is reported as a warning, not silently
dropped.

--args appends a GN build argument to args.gn; repeat it for more than one.
It exists because this script owns args.gn and rewrites it from scratch on
every run: an argument added to the file by hand (or with `gn args`) is gone
the next time anyone re-runs set.py, so a build argument such as
kernel_shell_enabled (//kernel/kernel_params.gni) is only really selectable if
it can be said here.  The value is GN syntax, so a string
keeps its quotes: --args 'sched_normal_discipline="fair"'.

--with names user programs the dev image carries in addition to its own, so
that several sessions can each build and boot their own image from their own
--out directory. It takes plain labels (//examples/foo); the image adds the
user toolchain. It adds programs without changing what boots: the image's boot
flow (userboot, then init, then the shell) is its own.

--autorun takes a command, `<BootFS name>[+arg...]`, that init runs once the
shell is up; it reaches the kernel command line as `ours.autorun.boot=`. The
name is the program's whole BootFS path, `bin/foo` for //examples/foo. It is
how a build runs its own program at boot, since --with only packages it. After
gn gen the command's program is checked against what the image actually
carries, and a directory that fails the check is put back the way it was.

Both are written into args.gn as the `ours` argument bundle
(//build/product/ours_args.gni).
"""

# PEP 604 annotations (`str | None`) are evaluated at definition time without
# this, and macOS still ships Python 3.9 as `python3` -- so `tools/set.py`, the
# documented way to configure a build directory, raised TypeError on import
# before running a single line of its own. The annotations stay as they are;
# this makes them strings until something asks for them.
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

VALID_CPUS = {
    "x86": "x86_64-elf",
    "arm": "aarch64-elf",
    "riscv": "riscv64-elf",
}

DEFAULT_PRODUCT = "bringup"

# The image that reads the `ours` bundle, and the two files `gn gen` writes for
# it that the --autorun check reads: the pack response file, which carries the
# image's `ours.autorun.boot=` entries, and the file list of its user BootFS
# (user_bootfs("bootfs"), //kernel/user_bootfs.gni), gathered from its deps'
# distribution entries. It is the list before the build expands it, the only
# one that exists after gn gen; a user program is a plain entry in it. Reading
# these instead of re-deriving names from labels keeps one source of truth:
# whatever the build decided the image carries is what gets checked.
OURS_IMAGE_RSP = "//kernel/products/image/dev:_obi.dev.rsp"
OURS_IMAGE_BOOTFS = "//kernel/products/image/dev:bootfs_dist_generate"
AUTORUN_ENTRY = "--entry=ours.autorun.boot="

# An autorun command is spliced into the kernel command line, where whitespace
# would split it into a second option and a quote would break the GN string
# that carries it; `+` separates its words.
AUTORUN_COMMAND = re.compile(r"[A-Za-z0-9._+/=:,-]+")


def list_products() -> list[str]:
    """List product names = products/<name>.gni files (sorted)."""
    products_dir = ROOT / "products"
    if not products_dir.is_dir():
        return []
    names = sorted(p.stem for p in products_dir.glob("*.gni") if p.is_file())
    return names


def _gn_path() -> Path:
    """Return the path to a prebuilt gn binary, or fall back to `gn` on PATH."""
    prebuilt = ROOT / "prebuilt" / "gn" / "gn"
    if prebuilt.is_file():
        return prebuilt
    gn = shutil.which("gn")
    if gn is None:
        raise SystemExit(
            "No `gn` binary found.  Place one at prebuilt/gn/gn or install it on PATH."
        )
    return Path(gn)


def _clang_tool_dir() -> str | None:
    """Resolve the clang bin directory to record as clang_tool_dir.

    Resolution order: the CLANG_TOOL_DIR environment variable, then any
    prebuilt/clang/*/bin directory that contains a `clang` binary and a
    sibling libc++ include tree (include/c++/v1).  Returns None when nothing
    usable is found; the generated build then falls back to the
    //build/config/compiler/clang.gni default (the same prebuilt path).
    """
    env = os.environ.get("CLANG_TOOL_DIR", "").strip()
    if env:
        candidate = Path(env)
        if (candidate / "clang").is_file():
            return str(candidate)

    for bin_dir in sorted((ROOT / "prebuilt" / "clang").glob("*/bin")):
        if (bin_dir / "clang").is_file() and (
            bin_dir.parent / "include" / "c++" / "v1"
        ).is_dir():
            return str(bin_dir)
    return None


def parse_target(spec: str) -> tuple[str, str]:
    """Split '<product>.<arch>' or bare '<arch>' into (product, arch)."""
    if "." in spec:
        product, arch = spec.split(".", 1)
        if not product:
            product = DEFAULT_PRODUCT
    else:
        product, arch = DEFAULT_PRODUCT, spec
    return product, arch


def autorun_program(command: str) -> str | None:
    """The BootFS name an autorun command starts: its first non-empty word.

    Mirrors how init splits `ours.autorun.boot` (init::AutorunCommand in
    system/service/init): words are separated by `+` and empty ones are
    skipped, so `+foo+bar` runs foo.
    """
    for word in command.split("+"):
        if word:
            return word
    return None


def ours_bundle(with_labels: list[str], autorun: str) -> list[str] | None:
    """Render --with/--autorun as the args.gn lines of the `ours` bundle.

    Returns None when neither was given, so the bundle stays at its empty
    default and the image carries and runs only its own programs. Only the
    fields that were given are written: args.gn replaces the default scope
    wholesale, and the image treats a missing field as "not asked for".

    Raises ValueError on input GN or the kernel command line would misread: a
    label must be absolute (a relative one would resolve against the image's
    BUILD.gn rather than against what the caller meant), and an autorun command
    must be one cmdline word that names a program.
    """
    if not with_labels and not autorun:
        return None
    for label in with_labels:
        if not label.startswith("//") or '"' in label:
            raise ValueError(f"--with '{label}' is not an absolute GN label (//path[:name])")
    if autorun and (not AUTORUN_COMMAND.fullmatch(autorun) or autorun_program(autorun) is None):
        raise ValueError(f"--autorun '{autorun}' is not <BootFS name>[+arg...]")

    lines = ["ours = {"]
    if with_labels:
        quoted = ", ".join(f"\"{label}\"" for label in with_labels)
        lines.append(f"  with = [ {quoted} ]")
    if autorun:
        lines.append(f"  autorun = \"{autorun}\"")
    lines.append("}")
    return lines


def check_autorun(gn: Path, out_dir: Path) -> str | None:
    """Return an error if the autorun command names a program the image lacks.

    Runs after `gn gen`, because both halves of the question are answers the
    build gives: which command the image's cmdline carries (the last
    `ours.autorun.boot`, since init takes the last), and which files the
    image's user BootFS actually carries (its file list, which --with
    shaped). Without the check the mismatch would surface only in QEMU, as
    `init: starting the autorun command = NotFound`.
    """
    result = subprocess.run(
        [str(gn), "outputs", str(out_dir), OURS_IMAGE_RSP, OURS_IMAGE_BOOTFS],
        cwd=str(ROOT), capture_output=True, text=True)
    if result.returncode != 0:
        return (f"cannot locate the image files for the --autorun check:\n"
                f"{result.stderr.strip()}")
    rsp_path, bootfs_path = (out_dir / p for p in result.stdout.split())

    command = None
    for line in rsp_path.read_text().splitlines():
        if line.startswith(AUTORUN_ENTRY):
            command = line[len(AUTORUN_ENTRY):]
    if command is None:
        return None
    program = autorun_program(command)

    entries = json.loads(bootfs_path.read_text())
    programs = sorted(entry["destination"] for entry in entries if "destination" in entry)
    if program not in programs:
        return (f"--autorun would start '{program}', but the image only carries "
                f"{', '.join(programs)}.\n"
                f"Add its label with --with, or run one of these.")
    return None


def set_build(product: str, arch: str, variants: list[str], out_dir: Path,
              tool_dir: str | None, extra_args: list[str],
              ours: list[str] | None = None) -> int:
    """Write args.gn and run gn gen.

    With an `ours` bundle, the result is also checked (check_autorun); on
    failure the directory is put back the way it was -- the previous args.gn
    regenerated, or a directory this run created removed -- so a bad --autorun
    never leaves behind a configured build of an image that runs nothing.
    """
    if ours is not None and any(a.partition("=")[0].strip() == "ours" for a in extra_args):
        print("error: --with/--autorun and --args ours=... both set the `ours` bundle; "
              "use one", file=sys.stderr)
        return 1
    if arch not in VALID_CPUS:
        print(f"error: unknown CPU '{arch}'.  Valid values: {', '.join(VALID_CPUS)}",
              file=sys.stderr)
        return 1
    if not (ROOT / "products" / f"{product}.gni").is_file():
        known = ", ".join(list_products()) or "(none)"
        print(f"error: unknown product '{product}'.  Known products: {known}",
              file=sys.stderr)
        return 1

    created_out_dir = not out_dir.exists()
    args_gn = out_dir / "args.gn"
    previous_args = args_gn.read_text() if args_gn.is_file() else None
    out_dir.mkdir(parents=True, exist_ok=True)

    lines = [
        "# Generated by tools/set.py — do not edit by hand.",
        f"target_cpu = \"{arch}\"",
        f"ours_product = \"{product}\"",
    ]
    if variants:
        quoted = ", ".join(f"\"{v}\"" for v in variants)
        lines.append(f"select_variant = [{quoted}]")
    if tool_dir:
        lines.append(f"clang_tool_dir = \"{tool_dir}\"")
    # Written verbatim as `name = value`: the value is GN syntax and GN is the
    # one to judge it, so a misspelt name or a bad value surfaces as gn gen's
    # own error below rather than as a second, weaker check here.
    for arg in extra_args:
        name, sep, value = arg.partition("=")
        if not sep or not name.strip() or not value.strip():
            print(f"error: --args '{arg}' is not <name>=<value>", file=sys.stderr)
            return 1
        lines.append(f"{name.strip()} = {value.strip()}")
    if ours is not None:
        lines.extend(ours)

    args_gn.write_text("\n".join(lines) + "\n")

    triple = VALID_CPUS[arch]
    variant_note = f", variants={variants}" if variants else ""
    if extra_args:
        variant_note += f", args={extra_args}"
    tool_note = f", clang={tool_dir}" if tool_dir else ", clang=PATH (no libc++ wiring!)"
    print(f"Generated {args_gn}  (product={product}, cpu={arch}, triple={triple}{variant_note}{tool_note})")
    if not tool_dir:
        print("note: no clang tool dir detected; the build uses the",
              "//build/config/compiler/clang.gni default (the prebuilt).",
              "Pass --clang-dir <path> or set CLANG_TOOL_DIR to override.",
              file=sys.stderr)

    gn = _gn_path()
    # --export-rust-project keeps rust-project.json fresh for
    # rust-analyzer (see //build/rust/BUILD.gn).
    cmd = [str(gn), "gen", str(out_dir), "--export-rust-project"]
    print(f"Running: {' '.join(cmd)}")
    result = subprocess.run(cmd, cwd=str(ROOT))
    if result.returncode == 0 and ours is not None:
        error = check_autorun(gn, out_dir)
        if error is not None:
            print(f"error: {error}", file=sys.stderr)
            _restore(gn, out_dir, created_out_dir, previous_args)
            return 1
    if result.returncode == 0:
        _refresh_debug_menu()
        print(f"Build:   ninja -C {out_dir} //products/{product}:{product}")
    return result.returncode


def _restore(gn: Path, out_dir: Path, created: bool, previous_args: str | None) -> None:
    """Undo a configuration that failed the --autorun check.

    A directory this run created is removed outright. An existing one gets its
    previous args.gn back and is regenerated, because build.ninja already
    reflects the rejected arguments and ninja would otherwise keep building
    them until something next touched args.gn.
    """
    if created:
        shutil.rmtree(out_dir)
        print(f"Removed {out_dir}; nothing was configured.", file=sys.stderr)
        return
    args_gn = out_dir / "args.gn"
    if previous_args is None:
        args_gn.unlink()
    else:
        args_gn.write_text(previous_args)
    subprocess.run([str(gn), "gen", str(out_dir), "--export-rust-project"],
                   cwd=str(ROOT), capture_output=True)
    print(f"Restored the previous configuration of {out_dir}.", file=sys.stderr)


def _refresh_debug_menu() -> None:
    """Keep the editor's F5 menu in step with the tree.

    VS Code cannot compute a pickString's options, so the three dropdowns in
    ours.code-workspace are literal lists; this is where they get rewritten, so
    that adding a product or a switchset shows up without anyone remembering.
    Here rather than in a build rule because the lists come from the source
    tree, not from a build directory -- and because this is the command people
    already run after changing the shape of the build.

    Never fatal: it is a convenience on top of a `gn gen` that has already
    succeeded, and failing the configure step over a menu would be backwards.
    """
    script = ROOT / "tools" / "gen_debug_menu.py"
    if not script.is_file():
        return
    try:
        subprocess.run([sys.executable, str(script), "--quiet"], cwd=str(ROOT),
                       check=False)
    except OSError as exc:
        print(f"note: could not refresh the debug menu: {exc}", file=sys.stderr)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Configure an Ours build directory (fx set equivalent).",
    )
    parser.add_argument(
        "target",
        nargs="?",
        help="<product>.<arch> or bare <arch> (default product: bringup)",
    )
    parser.add_argument("--variant", default="",
                        help="comma-separated variant names (e.g. lto,ubsan-trap)")
    parser.add_argument("--clang-dir", default="",
                        help="clang bin directory to record as clang_tool_dir "
                             "(default: CLANG_TOOL_DIR env, then prebuilt/clang/*/bin)")
    parser.add_argument("--list", action="store_true",
                        help="list valid products and arches")
    parser.add_argument("--out", default="",
                        help="output directory (default: out/<product>.<arch>)")
    parser.add_argument("--args", action="append", default=[], metavar="NAME=VALUE",
                        help="extra GN build argument written into args.gn "
                             "(repeatable; the value is GN syntax)")
    parser.add_argument("--with", dest="with_labels", action="append", default=[],
                        metavar="LABEL",
                        help="extra user program the dev image carries, as a plain "
                             "label (//examples/foo); repeatable. It is built and "
                             "packaged; the boot flow does not change")
    parser.add_argument("--autorun", default="", metavar="COMMAND",
                        help="<BootFS name>[+arg...] that init runs once the shell "
                             "is up (ours.autorun.boot, like zircon.autorun.boot); "
                             "the program must be in the image")
    args = parser.parse_args(argv)

    if args.list or args.target is None:
        print("Products:  " + (" ".join(list_products()) or "(none)"))
        print("Arches:    " + " ".join(VALID_CPUS))
        if args.target is None and not args.list:
            print("\nUsage: tools/set.py <product>.<arch> [--variant lto[,ubsan-trap]] "
                  "[--clang-dir <path>] [--out <dir>]")
            return 0
        return 0

    tool_dir = args.clang_dir or _clang_tool_dir()
    if args.clang_dir and not (Path(args.clang_dir) / "clang").is_file():
        print(f"error: --clang-dir '{args.clang_dir}' does not contain a `clang` binary",
              file=sys.stderr)
        return 1

    product, arch = parse_target(args.target)
    variants = [v.strip() for v in args.variant.split(",") if v.strip()]
    if args.out:
        out_dir = ROOT / args.out
    else:
        out_dir = ROOT / "out" / f"{product}.{arch}"
    try:
        ours = ours_bundle(args.with_labels, args.autorun)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return set_build(product, arch, variants, out_dir, tool_dir, args.args, ours)


if __name__ == "__main__":
    raise SystemExit(main())
