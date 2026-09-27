#!/usr/bin/env python3
"""Configure a build directory for a product and target CPU.

Usage:
  tools/set.py <arch>                 # e.g. tools/set.py x86 (default product: bringup)
  tools/set.py <product>.<arch>       # e.g. tools/set.py bringup.x86
  tools/set.py <product>.<arch> --variant lto[,ubsan-trap]
  tools/set.py <product>.<arch> --clang-dir <path>  # override the clang tool dir
  tools/set.py <product>.<arch> --args kernel_shell_enabled=false  # any GN arg
  tools/set.py <product>.<arch> --out out/mine --with //examples/foo --next foo
  tools/set.py --list                 # list valid products and arches

This is the equivalent of `fx set <product>.<board>`: it writes args.gn
with the selected product (ours_product), target CPU, and clang_tool_dir,
then runs gn gen.  The build directory defaults to out/<product>.<arch>
(e.g. out/bringup.x86); --out overrides it.  Build everything with:

  ninja -C out/<product>.<arch> //products/<product>:<product>

clang_tool_dir is resolved automatically (--clang-dir, then the
CLANG_TOOL_DIR environment variable, then prebuilt/clang/*/bin) and written
into args.gn.  The Ours kernel toolchains derive their libc++ include
directory from clang_tool_dir (see build/toolchain/ours/ours_toolchain.gni),
so a build generated without it cannot find the C++ standard library headers
(<cstdint>, <format>, ...) and the kernel compile fails at the first
include.  A missing clang tool dir is reported as a warning, not silently
dropped.

--args appends a GN build argument to args.gn, the way `fx set --args` does;
repeat it for more than one.  It exists because this script owns args.gn and
rewrites it from scratch on every run: an argument added to the file by hand
(or with `gn args`) is gone the next time anyone re-runs set.py, so a build
argument such as kernel_shell_enabled (//kernel/kernel_params.gni) is only
really selectable if it can be said here.  The value is GN syntax, so a string
keeps its quotes: --args 'sched_normal_discipline="fair"'.

--with and --next are the `fx set --with` counterpart: they say which user
programs the dev image carries and which one userboot starts next (the
image's `userboot.next=`; userboot itself always runs first), so that several
sessions can each build and boot their own image from their own --out
directory. They are written into args.gn as the `ours` argument bundle
(//build/product/ours_args.gni). --with REPLACES the image's default program
list rather than adding to it -- ninja only builds what the default target can
reach, so a program left out of the list is a program left out of the build --
and takes plain labels (//examples/foo); the image adds the user toolchain.
"""

# PEP 604 annotations (`str | None`) are evaluated at definition time without
# this, and macOS still ships Python 3.9 as `python3` -- so `tools/set.py`, the
# documented way to configure a build directory, raised TypeError on import
# before running a single line of its own. The annotations stay as they are;
# this makes them strings until something asks for them.
from __future__ import annotations

import argparse
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
# it that the --next check reads: the pack response file, which carries the
# image's effective `userboot.next=` entry, and the whitelist of user-visible
# BootFS names gathered from its deps' metadata. Reading these instead of
# re-deriving names from labels keeps one source of truth: whatever the build
# decided the image contains is what gets checked, defaults included.
OURS_IMAGE_RSP = "//kernel/products/image/dev:_obi.dev.rsp"
OURS_IMAGE_WHITELIST = "//kernel/products/image/dev:ours.user_visible"
NEXT_ENTRY = "--entry=userboot.next="

# The program userboot starts next is named by a BootFS file name spliced into
# the kernel command line, where whitespace would split it into a second option
# and a quote would break the GN string that carries it.
BOOTFS_NAME = re.compile(r"[A-Za-z0-9._+/-]+")


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


def ours_bundle(with_labels: list[str], next_program: str) -> list[str] | None:
    """Render --with/--next as the args.gn lines of the `ours` bundle.

    Returns None when neither flag was given, so the bundle stays at its empty
    default and every image keeps its own program list. Only the fields that
    were given are written: args.gn replaces the default scope wholesale, and
    the image fills a missing field with its own default, so writing a field
    the user did not ask for would silently pin a value that is not ours to
    choose.

    Raises ValueError on input GN would misread: a label must be absolute (a
    relative one would resolve against the image's BUILD.gn, not against the
    caller's intent), and a --next name must be a bare BootFS name.
    """
    if not with_labels and not next_program:
        return None
    for label in with_labels:
        if not label.startswith("//") or '"' in label:
            raise ValueError(f"--with '{label}' is not an absolute GN label (//path[:name])")
    if next_program and not BOOTFS_NAME.fullmatch(next_program):
        raise ValueError(f"--next '{next_program}' is not a BootFS file name")

    lines = ["ours = {"]
    if with_labels:
        quoted = ", ".join(f"\"{label}\"" for label in with_labels)
        lines.append(f"  with = [ {quoted} ]")
    if next_program:
        lines.append(f"  next = \"{next_program}\"")
    lines.append("}")
    return lines


def check_next(gn: Path, out_dir: Path) -> str | None:
    """Return an error if the program userboot starts next is not in the image.

    Runs after `gn gen`, because both halves of the question are answers the
    build gives: which name the image's command line hands userboot as
    `userboot.next` (whatever --next said, or the image's default), and which
    user programs the image actually carries (the whitelist its deps' metadata
    produce, which --with shaped). A mismatch is an image in which userboot
    looks for a program that is not there -- caught here rather than in QEMU.
    """
    result = subprocess.run(
        [str(gn), "outputs", str(out_dir), OURS_IMAGE_RSP, OURS_IMAGE_WHITELIST],
        cwd=str(ROOT), capture_output=True, text=True)
    if result.returncode != 0:
        return (f"cannot locate the image files for the --next check:\n"
                f"{result.stderr.strip()}")
    rsp_path, whitelist_path = (out_dir / p for p in result.stdout.split())

    next_program = None
    for line in rsp_path.read_text().splitlines():
        if line.startswith(NEXT_ENTRY):
            next_program = line[len(NEXT_ENTRY):]
    if next_program is None:
        return f"{rsp_path} has no {NEXT_ENTRY}<name> entry"

    programs = whitelist_path.read_text().split()
    if next_program not in programs:
        return (f"userboot would start '{next_program}' next, but the image only carries "
                f"{', '.join(programs)}.\n"
                f"Add its label with --with, or name one of these with --next.")
    return None


def set_build(product: str, arch: str, variants: list[str], out_dir: Path,
              tool_dir: str | None, extra_args: list[str],
              ours: list[str] | None = None) -> int:
    """Write args.gn and run gn gen.

    With an `ours` bundle, the result is also checked (check_next); on failure
    the directory is put back the way it was -- the previous args.gn
    regenerated, or a directory this run created removed -- so a bad
    --with/--next pair never leaves behind a configured build of an image that
    cannot boot.
    """
    if ours is not None and any(a.partition("=")[0].strip() == "ours" for a in extra_args):
        print("error: --with/--next and --args ours=... both set the `ours` bundle; "
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
        error = check_next(gn, out_dir)
        if error is not None:
            print(f"error: {error}", file=sys.stderr)
            _restore(gn, out_dir, created_out_dir, previous_args)
            return 1
    if result.returncode == 0:
        _refresh_debug_menu()
        print(f"Build:   ninja -C {out_dir} //products/{product}:{product}")
    return result.returncode


def _restore(gn: Path, out_dir: Path, created: bool, previous_args: str | None) -> None:
    """Undo a configuration that failed the --next check.

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
                        help="user program the dev image carries, as a plain label "
                             "(//examples/foo); repeatable, and replaces the image's "
                             "default list rather than adding to it")
    parser.add_argument("--next", dest="next_program", default="", metavar="NAME",
                        help="BootFS name of the program userboot starts next, "
                             "i.e. the image's userboot.next= (default: the image's "
                             "own choice); userboot itself always runs first")
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
        ours = ours_bundle(args.with_labels, args.next_program)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return set_build(product, arch, variants, out_dir, tool_dir, args.args, ours)


if __name__ == "__main__":
    raise SystemExit(main())
