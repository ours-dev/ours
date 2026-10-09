#!/usr/bin/env python3
"""Make llvm-cov, llvm-profdata and the profile runtime available for the compiler in the tree.

    python3 scripts/fetch_llvm_coverage.py [--clang-dir DIR] [--base-url URL] [--force]

Source-based coverage needs three things the vendored compilers do not carry:
`llvm-profdata` merges raw profiles, `llvm-cov` turns them into a report, and
the profile runtime (libclang_rt.profile_osx.a on macOS) is what the clang
driver links into a program built with -fprofile-instr-generate.

Nothing here is pinned. A coverage map can only be read by the LLVM release
that wrote it, so the compiler decides: the script runs `clang --version`
(`clang version 23.1.0-rc3 (https://github.com/llvm/llvm-project <commit>)`),
turns the version into the release tag `llvmorg-23.1.0-rc3` and the host into
upstream's archive name, and downloads that. When the compiler is replaced, the
tools in place no longer report its version, so the next run fetches again; no
edit to this file is involved.

The compiler it looks at is the first of prebuilt/clang/llvm-* (highest number
first) whose bin/clang runs on this host, so on macOS that is llvm-23 and in the
dev container llvm-22. When the tools already work for it, including the case
where they sit next to the real compiler that bin/clang points into, nothing is
downloaded.

Upstream publishes only the whole toolchain, 1.5 GB or more. The archive is
saved in a scratch directory inside the compiler's own, checked, mined for the
files wanted, and deleted. Before anything is installed the script reads the
archive's own compiler and requires it to carry the same source commit as the
one in the tree, and runs the staged tools to see that they report the same
release. Nothing is installed until both checks and the whole download have
succeeded. There is no hash recorded in this repository (that is what pinning
would be); the download is protected by HTTPS and by the xz stream's own CRC.

The runtime is installed into the compiler's resource directory, so the driver
finds it by itself, and only when that directory is inside the compiler's tree:
this never writes into a system directory. On Linux the runtime is not required,
since the distribution's compiler package ships it in its own layout.
"""
from __future__ import annotations

import argparse
import os
import platform
import posixpath
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parent.parent
CLANG_ROOT = ROOT / "prebuilt" / "clang"

RELEASE_URL = "https://github.com/llvm/llvm-project/releases/download"

TOOLS = ("llvm-cov", "llvm-profdata")

# What `clang --version` prints for an upstream build, and for a distribution's.
CLANG_VERSION = re.compile(r"clang version (\S+)")
SOURCE_COMMIT = re.compile(r"github\.com/llvm/llvm-project ([0-9a-f]{40})")
RELEASE_VERSION = re.compile(r"\d+\.\d+\.\d+(-rc\d+)?")
# What `llvm-cov --version` prints: "LLVM version 23.1.0-rc3", with any
# distribution's name in front.
TOOL_VERSION = re.compile(r"LLVM version (\S+)")

# Free space wanted beyond the archive itself: the files kept, and slack.
SPACE_MARGIN = 512 * 1024 * 1024

# The archive's compiler is searched in chunks of this size.
CHUNK = 4 * 1024 * 1024


class FetchError(Exception):
    """The tools could not be made available, or what was downloaded did not
    match the compiler in the tree."""


@dataclass(frozen=True)
class Compiler:
    directory: Path
    version: str
    # The upstream source commit the compiler was built from, when it says.
    commit: Optional[str]

    @property
    def clang(self) -> Path:
        return self.directory / "bin" / "clang"

    @property
    def major(self) -> str:
        return self.version.split(".")[0]


@dataclass(frozen=True)
class Host:
    # The platform part of upstream's archive name: LLVM-<version>-<asset>.tar.xz
    asset: str
    # Where upstream puts the profile runtime, under lib/clang/<major>/lib/.
    runtime_dir: str
    runtime_name: str
    # Whether the tree's compiler needs the runtime installed to link at all.
    runtime_required: bool


def detect_host() -> Host:
    system = platform.system()
    machine = platform.machine().lower()
    if system == "Darwin" and machine == "arm64":
        return Host("macOS-ARM64", "darwin", "libclang_rt.profile_osx.a", True)
    if system == "Linux" and machine in ("x86_64", "amd64"):
        return Host("Linux-X64", "x86_64-unknown-linux-gnu", "libclang_rt.profile.a", False)
    if system == "Linux" and machine in ("aarch64", "arm64"):
        return Host("Linux-ARM64", "aarch64-unknown-linux-gnu", "libclang_rt.profile.a", False)
    raise FetchError(f"upstream publishes no LLVM archive for {system} {machine}")


def probe(directory: Path) -> Optional[Compiler]:
    """The compiler under `directory`, or None if its bin/clang does not run here
    (a broken symlink, or a binary built for another OS)."""
    clang = directory / "bin" / "clang"
    try:
        result = subprocess.run([str(clang), "--version"], capture_output=True, text=True,
                                timeout=60)
    except (OSError, subprocess.SubprocessError):
        return None
    if result.returncode != 0:
        return None
    version = CLANG_VERSION.search(result.stdout)
    if not version:
        return None
    commit = SOURCE_COMMIT.search(result.stdout)
    return Compiler(directory, version.group(1), commit.group(1) if commit else None)


def find_compiler(explicit: Optional[Path]) -> Compiler:
    if explicit is not None:
        candidates = [explicit]
    else:
        candidates = sorted(CLANG_ROOT.glob("llvm-*"),
                            key=lambda path: [int(n) for n in re.findall(r"\d+", path.name)],
                            reverse=True)
    for directory in candidates:
        compiler = probe(directory)
        if compiler is not None:
            return compiler
    where = explicit if explicit is not None else CLANG_ROOT
    raise FetchError(f"no compiler under {where} runs on this host")


def resource_dir(compiler: Compiler) -> Path:
    result = subprocess.run([str(compiler.clang), "--print-resource-dir"], check=True,
                            capture_output=True, text=True, timeout=60)
    return Path(result.stdout.strip())


def runtime_path(compiler: Compiler, host: Host) -> Path:
    return resource_dir(compiler) / "lib" / host.runtime_dir / host.runtime_name


def locate_tool(compiler: Compiler, tool: str) -> Optional[Path]:
    """The tool next to the compiler, or next to the real compiler bin/clang
    points into (the dev container's bin/ holds only symlinks to some tools)."""
    directories = [compiler.directory / "bin", compiler.clang.resolve().parent]
    for directory in directories:
        path = directory / tool
        if path.is_file():
            return path
    return None


def reported_release(tool: Path) -> Optional[str]:
    try:
        result = subprocess.run([str(tool), "--version"], capture_output=True, text=True,
                                timeout=60)
    except (OSError, subprocess.SubprocessError):
        return None
    match = TOOL_VERSION.search(result.stdout + result.stderr)
    return match.group(1) if match else None


def problems(compiler: Compiler, host: Host) -> list[str]:
    """What is missing or does not belong to this compiler. Empty means ready."""
    found = []
    for tool in TOOLS:
        path = locate_tool(compiler, tool)
        if path is None:
            found.append(f"{tool} is not next to the compiler")
            continue
        release = reported_release(path)
        if release != compiler.version:
            found.append(f"{path} reports LLVM {release}, the compiler is {compiler.version}")
    if host.runtime_required:
        runtime = runtime_path(compiler, host)
        if not runtime.is_file():
            found.append(f"{runtime} is missing")
    return found


def archive_name(compiler: Compiler, host: Host) -> str:
    return f"LLVM-{compiler.version}-{host.asset}"


def check_fetchable(compiler: Compiler) -> None:
    if compiler.commit is None or not RELEASE_VERSION.fullmatch(compiler.version):
        raise FetchError(
            f"{compiler.clang} reports version {compiler.version} without an upstream "
            f"release commit, so there is no release to fetch matching tools from. "
            f"Install llvm-cov and llvm-profdata of exactly that build next to it.")


def download(url: str, path: Path, say: Callable[[str], None]) -> None:
    request = Request(url, headers={"User-Agent": "ours-fetch-llvm-coverage/1"})
    with urlopen(request, timeout=60) as response, path.open("wb") as output:
        total = int(response.headers.get("Content-Length") or 0)
        free = shutil.disk_usage(path.parent).free
        if total and free < total + SPACE_MARGIN:
            raise FetchError(f"{url} is {total >> 20} MiB and only {free >> 20} MiB is free "
                             f"on the disk holding {path.parent}")
        done = 0
        next_report = 0
        for chunk in iter(lambda: response.read(1024 * 1024), b""):
            output.write(chunk)
            done += len(chunk)
            if done >= next_report:
                say(f"  {done >> 20} MiB of {total >> 20} MiB" if total else f"  {done >> 20} MiB")
                next_report += 128 * 1024 * 1024
        if total and done != total:
            raise FetchError(f"{url}: got {done} bytes of {total}")


def contains(stream, needle: bytes) -> bool:
    """Whether `needle` occurs anywhere in the bytes of `stream`, including across
    the boundary of two reads."""
    keep = len(needle) - 1
    tail = b""
    while True:
        data = stream.read(CHUNK)
        if not data:
            return False
        window = tail + data
        if needle in window:
            return True
        tail = window[-keep:] if keep else b""


def resolve(archive: tarfile.TarFile, top: str, name: str) -> tarfile.TarInfo:
    """The regular file `name` is, following the links upstream's archive uses
    (bin/clang is a link to bin/clang-23). Anything leading outside `top` is refused."""
    try:
        member = archive.getmember(name)
        for _ in range(8):
            if member.issym():
                target = posixpath.normpath(
                    posixpath.join(posixpath.dirname(member.name), member.linkname))
            elif member.islnk():
                target = posixpath.normpath(member.linkname)
            else:
                break
            if not target.startswith(top + "/"):
                raise FetchError(f"{name}: link to {member.linkname!r} leaves {top}/")
            member = archive.getmember(target)
        else:
            raise FetchError(f"{name}: too many links")
    except KeyError as error:
        raise FetchError(f"{top}.tar.xz has no {name!r}") from error
    if not member.isfile():
        raise FetchError(f"{name} is not a regular file in the archive")
    return member


def extract(archive: tarfile.TarFile, member: tarfile.TarInfo, destination: Path,
            mode: int) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    source = archive.extractfile(member)
    if source is None:
        raise FetchError(f"{member.name}: cannot be read")
    with source, destination.open("wb") as output:
        shutil.copyfileobj(source, output, 1024 * 1024)
    destination.chmod(mode)


def fetch(compiler: Compiler, host: Host, base_url: str, say: Callable[[str], None]) -> None:
    check_fetchable(compiler)
    assert compiler.commit is not None
    name = archive_name(compiler, host)
    url = f"{base_url}/llvmorg-{compiler.version}/{name}.tar.xz"

    # Where the files end up, and where upstream keeps them.
    runtime_target = runtime_path(compiler, host)
    try:
        runtime_target.resolve().relative_to(compiler.directory.resolve())
        install_runtime = True
    except ValueError:
        if host.runtime_required:
            raise FetchError(f"the compiler looks for its runtime in {runtime_target}, which "
                             f"is outside {compiler.directory}; refusing to write there")
        install_runtime = False
    wanted = [(f"bin/{tool}", compiler.directory / "bin" / tool, 0o755) for tool in TOOLS]
    if install_runtime:
        runtime_source = f"lib/clang/{compiler.major}/lib/{host.runtime_dir}/{host.runtime_name}"
        wanted.append((runtime_source, runtime_target, 0o644))

    with tempfile.TemporaryDirectory(prefix=".fetch-llvm-coverage-",
                                     dir=compiler.directory) as scratch_name:
        scratch = Path(scratch_name)
        archive_path = scratch / f"{name}.tar.xz"
        say(f"Downloading {url}")
        try:
            download(url, archive_path, say)
        except OSError as error:
            raise FetchError(f"cannot download {url}: {error}") from error

        staged = []
        say("Checking the archive and taking out what is needed (this decompresses all of it)")
        try:
            with tarfile.open(archive_path, "r:xz") as archive:
                clang = resolve(archive, name, f"{name}/bin/clang")
                with archive.extractfile(clang) as stream:
                    if not contains(stream, compiler.commit.encode()):
                        raise FetchError(
                            f"the archive's compiler was not built from commit "
                            f"{compiler.commit}, which {compiler.clang} reports; "
                            f"tools from it could not read this compiler's coverage maps")
                for relative, destination, mode in wanted:
                    try:
                        member = resolve(archive, name, f"{name}/{relative}")
                    except FetchError:
                        if destination == runtime_target and not host.runtime_required:
                            continue
                        raise
                    stage = scratch / "stage" / relative
                    extract(archive, member, stage, mode)
                    staged.append((stage, destination))
        except (tarfile.TarError, EOFError, OSError) as error:
            raise FetchError(f"{name}.tar.xz: cannot be read: {error}") from error

        for stage, destination in staged:
            if destination.name in TOOLS:
                release = reported_release(stage)
                if release != compiler.version:
                    raise FetchError(f"{destination.name} from the archive reports LLVM "
                                     f"{release}, the compiler is {compiler.version}")
        for stage, destination in staged:
            destination.parent.mkdir(parents=True, exist_ok=True)
            os.replace(stage, destination)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--clang-dir", type=Path,
                        help="the compiler directory to serve (default: the first of "
                             "prebuilt/clang/llvm-* that runs on this host)")
    parser.add_argument("--base-url", default=RELEASE_URL,
                        help="where release archives are downloaded from, as "
                             "<base>/llvmorg-<version>/LLVM-<version>-<host>.tar.xz")
    parser.add_argument("--force", action="store_true",
                        help="download and install again even if the tools are in place")
    args = parser.parse_args(argv)

    def say(message: str) -> None:
        print(message, flush=True)

    try:
        host = detect_host()
        compiler = find_compiler(args.clang_dir)
        say(f"Compiler: {compiler.clang} (LLVM {compiler.version})")
        missing = problems(compiler, host)
        if not missing and not args.force:
            say("Already available:")
        else:
            for line in missing:
                say(f"  {line}")
            fetch(compiler, host, args.base_url, say)
            say("Installed:")
        for tool in TOOLS:
            say(f"  {locate_tool(compiler, tool)}")
        if host.runtime_required:
            say(f"  {runtime_path(compiler, host)}")
        return 0
    except (FetchError, subprocess.SubprocessError, OSError) as error:
        print(f"fetch_llvm_coverage.py: error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
