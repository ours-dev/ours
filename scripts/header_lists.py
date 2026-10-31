#!/usr/bin/env python3
"""Does each cpp_interface target list the headers of its include root?

    python3 scripts/header_lists.py [OUT] [--scope PREFIX]... [--all]

A cpp_interface() (build/cpp/cpp_interface.gni) publishes an include root and
lists headers in `public`, and the list is metadata for GN and nothing else:
ninja never reads it, so a header missing from it, or a name in it that no
longer exists, breaks no build. What reads it is `gn check`, and only for a
header some target lists ("includes matching unknown paths are ignored"), so a
header that no target lists is a header whose includers are never held to a
dependency. That is how x86's headers target came to list one file of 25
(docs/changes/261030/kernel-arch-seam.md): nothing failed, and the other 24
were invisible to the checker.

This asks GN for the targets, with no list kept here:

  - a cpp_interface is a source_set that compiles nothing (no `sources`), has
    an explicit `public` list, and publishes a config named for itself,
    `<label>.config` (`gn desc //*`); the template makes exactly that;
  - its include root is that config's one include directory (`gn desc` of the
    config, in the target's toolchain);
  - the targets that publish one root, in one toolchain, are taken together: a
    library may split a root between several (sampler, phys), and then the
    union of their lists is what has to cover it.

Two things are wrong, and both are named by file:

  missing  a header under an include root that none of the targets publishing
           it lists; a header is a file named like one (HEADER_SUFFIXES), so
           the BUILD.gn or .gni that a root such as //system/public/ holds is
           not one, and dotfiles are not counted
  stray    an entry of a target's list that is not a file under that target's
           include root: a name that does not exist, or a real file outside
           the root (the usual spelling slip, and what a rename leaves)

SCOPE is a label prefix; by default only the architectures' targets are held to
this, because the rest of the tree has long-standing gaps that are a repair
item of their own (docs/repairs/261030/build-headers.md). They are every target
under `//kernel/arch/` (each CPU's own headers, and its half of the <arch/...>
include namespace under `kernel/arch/<cpu>/lib/arch`) and the generic half of
that namespace, `//kernel/lib/arch:`, so that the two halves of one namespace
are held to the same rule. --all checks every target and shows that debt. A
scope that matches no cpp_interface is an error, not a pass: a mistyped prefix
would otherwise report that every one of zero headers is listed. A label is
`//path/to/dir:name`, so a prefix that ends in `/` reaches the targets of that
directory's subdirectories, and one that ends in `:` the targets of that
directory alone (`//kernel/mm:` is mm's, `//kernel/mm/` is not).

Used by the `ours-verify` skill's boot_check.py: its x86 mode runs it on the
build directory beside the host tests, and its riscv mode (riscv_boot.py) on
the riscv one after the boot, because each directory's graph holds targets the
other's does not (the riscv directory alone has //kernel/arch/riscv/{kernel,
mm,phys}). It works on any generated build directory of the checkout the
script is in, whatever the working directory.
"""
# macOS still ships Python 3.9 as python3; this keeps the annotations lazy.
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import time
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path

# The architectures' targets: all of //kernel/arch/, and libarch's generic half,
# the one target of //kernel/lib/arch (a prefix ending in a colon reaches that
# directory's targets and not those of its subdirectories).
DEFAULT_SCOPE = ("//kernel/arch/", "//kernel/lib/arch:")
# .tcc is a template's out-of-line half, which its header includes (ktl/xarray.tcc).
HEADER_SUFFIXES = (".h", ".hh", ".hpp", ".hxx", ".inc", ".inl", ".ipp", ".tcc")
ALL = ("//",)
GN_JOBS = 8  # `gn desc` calls in flight, one per target to find the root of
SHOWN = 20  # problems printed under the summary line


class HeaderListsError(RuntimeError):
    """GN could not say what the include roots and their lists are."""


@dataclass(frozen=True)
class Problem:
    kind: str  # "missing" or "stray"
    path: str  # the file, as a GN source-absolute path
    targets: tuple[str, ...]  # labels without toolchain: who publishes the root, or who lists it
    root: str  # the include root, GN path

    def describe(self) -> str:
        targets = ", ".join(self.targets)
        if self.kind == "missing":
            return f"missing {self.path}: under the include root of {targets}, which does not list it"
        return f"stray   {self.path}: listed by {targets}, and not a file under its include root {self.root}"


@dataclass
class Result:
    problems: list[Problem]
    roots: int  # distinct include roots examined
    targets: int  # distinct target labels examined
    headers: int  # distinct files under those roots
    scope: tuple[str, ...]
    seconds: float = 0.0

    @property
    def ok(self) -> bool:
        return not self.problems


def repo_root() -> Path:
    """The checkout this script is in: its BUILD.gn files are the ones its
    build directory was generated from. Not `git rev-parse` of the working
    directory, which is another checkout's root when the script is run from
    there, and no root at all outside a repository."""
    # scripts/header_lists.py -> repo root
    return Path(__file__).resolve().parents[1]


def gn(root: Path, *args: str) -> str:
    try:
        result = subprocess.run([str(root / "prebuilt/gn/gn"), *args], cwd=root,
                                capture_output=True, text=True)
    except OSError as error:
        raise HeaderListsError(f"cannot run prebuilt/gn/gn: {error}")
    if result.returncode != 0:
        raise HeaderListsError(f"gn {args[0]}: {(result.stdout + result.stderr).strip()[-500:]}")
    return result.stdout


def gn_json(root: Path, *args: str) -> dict:
    try:
        return json.loads(gn(root, *args, "--format=json"))
    except json.JSONDecodeError as error:
        raise HeaderListsError(f"gn {args[0]}: output is not JSON ({error})")


def without_toolchain(label: str) -> str:
    return re.sub(r"\(.*\)$", "", label)


def interfaces(targets: dict, scope: tuple[str, ...]) -> dict[str, tuple[str, str]]:
    """label -> (its own config's label, its toolchain) for each cpp_interface
    in scope, by the shape the template gives it."""
    found = {}
    for label, target in targets.items():
        if target.get("type") != "source_set" or not label.startswith(scope):
            continue
        own = without_toolchain(label) + ".config"
        # `public` is the string "*" unless the target set it, and a library
        # that compiles sources is not a header interface even if it publishes
        # a root.
        if (isinstance(target.get("public"), list) and not target.get("sources")
                and own in target.get("public_configs", [])):
            found[label] = (own, target["toolchain"])
    return found


def roots_of(root: Path, out: Path, shapes: dict[str, tuple[str, str]]) -> dict[str, str]:
    """label -> its include root, a GN path ending in /: the one directory its
    own config publishes."""
    def one(item: tuple[str, tuple[str, str]]) -> tuple[str, str]:
        label, (config, toolchain) = item
        dirs = gn_json(root, "desc", str(out), f"{config}({toolchain})").get(config, {}).get("include_dirs", [])
        if len(dirs) != 1:
            raise HeaderListsError(f"{label}: its config {config} publishes {len(dirs)} include "
                                   f"directories, and an include root is one")
        return label, dirs[0]

    with ThreadPoolExecutor(GN_JOBS) as pool:
        return dict(pool.map(one, shapes.items()))


def files_under(root: Path, gn_dir: str) -> set[str]:
    """Every regular file under a GN directory, as GN paths. Dotfiles (a macOS
    .DS_Store, an editor's backup) are left out."""
    base = root / gn_dir[2:]
    if not base.is_dir():
        return set()
    return {"//" + path.relative_to(root).as_posix() for path in base.rglob("*")
            if path.is_file() and not path.name.startswith(".")}


def compare(root: Path, targets: dict, roots: dict[str, str]) -> Result:
    """Compare each root's files with the lists of the targets publishing it, in
    each toolchain. A file is one problem however many toolchains see it, and
    however many targets publish its root in each: the publishers of the
    toolchains are merged into the one finding."""
    shared: dict[tuple[str, str], list[str]] = defaultdict(list)  # (root, toolchain) -> labels
    for label, path in roots.items():
        shared[(path, targets[label]["toolchain"])].append(label)

    missing: dict[str, set[str]] = defaultdict(set)  # file -> who publishes a root it is unlisted in
    missing_root: dict[str, str] = {}  # file -> the first root found it was missing from
    stray: dict[tuple[str, str], str] = {}  # (entry, the target listing it) -> that target's root
    examined_roots, examined_targets, headers = set(), set(), set()
    for (path, _), labels in sorted(shared.items()):
        on_disk = files_under(root, path)
        examined_roots.add(path)
        under = {entry for entry in on_disk if entry.endswith(HEADER_SUFFIXES)}
        headers |= under
        listed: set[str] = set()
        for label in labels:
            examined_targets.add(without_toolchain(label))
            public = set(targets[label]["public"])
            listed |= public
            for entry in public - on_disk:
                stray.setdefault((entry, without_toolchain(label)), path)
        who = {without_toolchain(label) for label in labels}
        for entry in under - listed:
            missing[entry] |= who
            missing_root.setdefault(entry, path)

    problems = [Problem("stray", entry, (lister,), path) for (entry, lister), path in stray.items()]
    problems += [Problem("missing", entry, tuple(sorted(who)), missing_root[entry])
                 for entry, who in missing.items()]
    return Result(sorted(problems, key=lambda p: (p.kind != "missing", p.path, p.targets)),
                  len(examined_roots), len(examined_targets), len(headers), ())


def check(root: Path, out: Path, scope: tuple[str, ...] = DEFAULT_SCOPE) -> Result:
    """Compare every in-scope cpp_interface's list with its include root, in every
    toolchain `out` builds it in. `out` is a generated GN build directory."""
    start = time.monotonic()
    targets = gn_json(root, "desc", str(out), "//*")
    shapes = interfaces(targets, scope)
    if not shapes:
        # Comparing nothing finds nothing, and "every one listed" of zero
        # headers is not a pass: a mistyped --scope, a moved //kernel/arch or
        # a changed template shape would all look like one.
        raise HeaderListsError(f"no cpp_interface target has a label under {' or '.join(scope)}, "
                               f"so there is no list to compare (a mistyped --scope prefix, or "
                               f"the template's shape changed)")
    result = compare(root, targets, roots_of(root, out, shapes))
    result.scope = scope
    result.seconds = time.monotonic() - start
    return result


def summary(result: Result) -> str:
    scope = " and ".join(result.scope)
    counts = f"{result.targets} targets, {result.roots} include roots, {result.headers} headers under {scope}"
    if result.ok:
        return f"{counts}, every one listed · {result.seconds:.1f}s"
    missing = sum(1 for p in result.problems if p.kind == "missing")
    return f"{missing} missing, {len(result.problems) - missing} stray among {counts} · {result.seconds:.1f}s"


def report(root: Path, out: Path, scope: tuple[str, ...] = DEFAULT_SCOPE) -> bool:
    """Print boot_check's one line for the check, and what it found under it."""
    try:
        result = check(root, out, scope)
    except RuntimeError as error:
        print(f"{'headers':<10} FAIL  {error}")
        return False
    print(f"{'headers':<10} {'PASS' if result.ok else 'FAIL'}  {summary(result)}")
    for problem in result.problems[:SHOWN]:
        print(f"  {problem.describe()}")
    if len(result.problems) > SHOWN:
        print(f"  ... and {len(result.problems) - SHOWN} more")
    return result.ok


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("out", nargs="?", default="out/bringup.x86",
                        help="generated GN build directory (default: out/bringup.x86)")
    parser.add_argument("--scope", action="append", metavar="PREFIX",
                        help="check the targets whose label starts with PREFIX "
                             f"(repeatable; default: {' '.join(DEFAULT_SCOPE)})")
    parser.add_argument("--all", action="store_true", help="check every target, not only the architectures'")
    args = parser.parse_args(argv)
    root = repo_root()
    scope = ALL if args.all else tuple(args.scope or DEFAULT_SCOPE)
    return 0 if report(root, (root / args.out).resolve(), scope) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
