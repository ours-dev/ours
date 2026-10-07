#!/usr/bin/env python3
"""Fetch the pinned ccache release into prebuilt/ccache/.

    python3 scripts/fetch_ccache.py [--force]

ccache is the compiler cache the build can put in front of every compile, so
that a source one worktree has compiled is not compiled again in another. This
downloads upstream's macOS release (one universal binary, arm64 and x86_64),
checks it against the SHA-256 pinned below, and unpacks it into
prebuilt/ccache/, which then holds prebuilt/ccache/ccache and its manual and
licence. A checkout under .worktrees/ links prebuilt/ to the project root's,
so one fetch serves every worktree.

Does nothing when prebuilt/ccache/ already holds the pinned archive, unless
--force is given. Nothing in prebuilt/ccache/ changes until the download has
matched the pinned hash and unpacked in full.
"""
from __future__ import annotations

import argparse
import hashlib
import platform
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path
from urllib.request import Request, urlopen

# The pinned release; VERSION and ARCHIVE_SHA256 change together.
# ARCHIVE_SHA256 is the digest GitHub lists for the release asset
# (`gh api repos/ccache/ccache/releases/tags/v4.14.1`, field `digest`). It
# equals `shasum -a 256` of the downloaded archive, whose minisign signature
# (the asset's `.minisig`) verifies under the public key ccache.dev publishes
# on its download page (key id 7C55E705DB25EF17). Repeat all three checks
# before pinning another version.
VERSION = "4.14.1"
ARCHIVE = f"ccache-{VERSION}-darwin.tar.gz"
ARCHIVE_URL = f"https://github.com/ccache/ccache/releases/download/v{VERSION}/{ARCHIVE}"
ARCHIVE_SHA256 = "c279fa81e2e806b4d9e64d4a06cb569a84ef54151feef796e5d4f4e95a464563"

# Every member of the archive sits under this directory; it is stripped, so
# the binary lands at prebuilt/ccache/ccache.
ARCHIVE_TOP = f"ccache-{VERSION}-darwin"

ROOT = Path(__file__).resolve().parent.parent
DESTINATION = ROOT / "prebuilt" / "ccache"

# Written last into the unpacked tree. It names the archive the tree came
# from, so a later run can tell the pinned release is already in place
# without running the binary.
STAMP = ".archive"


class FetchError(Exception):
    """The archive could not be downloaded, did not match the pin, or did not
    unpack into the expected layout."""


def stamp_text() -> str:
    return f"{ARCHIVE} sha256:{ARCHIVE_SHA256}\n"


def is_current(destination: Path) -> bool:
    try:
        return (destination / STAMP).read_text(encoding="utf-8") == stamp_text()
    except OSError:
        return False


def download(url: str, path: Path) -> str:
    """Saves `url` to `path` and returns the SHA-256 of what was saved."""
    digest = hashlib.sha256()
    request = Request(url, headers={"User-Agent": "ours-fetch-ccache/1"})
    with urlopen(request, timeout=60) as response, path.open("wb") as output:
        for chunk in iter(lambda: response.read(1024 * 1024), b""):
            digest.update(chunk)
            output.write(chunk)
    return digest.hexdigest()


def stripped_members(archive: tarfile.TarFile) -> list[tarfile.TarInfo]:
    """The archive's members, renamed relative to ARCHIVE_TOP.

    The hash already ties the archive to upstream's release, but the unpack
    still refuses anything that could write outside the staging directory or
    leave more than plain files behind: an absolute path, a `..`, a link or a
    device. The interpreter this runs under may predate tarfile's extraction
    filters, so the check is done here.
    """
    members = []
    for member in archive.getmembers():
        parts = Path(member.name).parts
        if member.name.startswith("/") or ".." in parts:
            raise FetchError(f"{ARCHIVE}: unsafe path {member.name!r}")
        if not parts or parts[0] != ARCHIVE_TOP:
            raise FetchError(f"{ARCHIVE}: {member.name!r} is outside {ARCHIVE_TOP}/")
        if not (member.isfile() or member.isdir()):
            raise FetchError(f"{ARCHIVE}: {member.name!r} is not a plain file or directory")
        if len(parts) == 1:
            continue
        member.name = str(Path(*parts[1:]))
        member.mode &= 0o755
        members.append(member)
    return members


def unpack(archive_path: Path, staging: Path) -> None:
    with tarfile.open(archive_path, "r:gz") as archive:
        members = stripped_members(archive)
        if hasattr(tarfile, "data_filter"):
            archive.extractall(staging, members=members, filter="data")
        else:
            archive.extractall(staging, members=members)
    if not (staging / "ccache").is_file():
        raise FetchError(f"{ARCHIVE}: no {ARCHIVE_TOP}/ccache in the archive")


def install(staging: Path, destination: Path, scratch: Path) -> None:
    """Moves `staging` to `destination`. An older tree there is moved into
    `scratch` first, which is deleted with the rest of the scratch directory.
    Both renames stay inside prebuilt/, so neither copies anything."""
    if destination.exists():
        destination.rename(scratch / "previous")
    staging.rename(destination)


def reported_version(binary: Path) -> str:
    result = subprocess.run([str(binary), "--version"], check=True, capture_output=True,
                            text=True)
    return result.stdout.splitlines()[0] if result.stdout else ""


def fetch(force: bool) -> int:
    if platform.system() != "Darwin":
        raise FetchError(f"only upstream's macOS archive is pinned; this host is "
                         f"{platform.system()}. Pin that platform's archive and hash first.")
    if not force and is_current(DESTINATION):
        print(f"{DESTINATION}: {ARCHIVE} already in place")
        return 0

    prebuilt = DESTINATION.parent.resolve()
    prebuilt.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".fetch-ccache-", dir=prebuilt) as scratch_name:
        scratch = Path(scratch_name)
        archive_path = scratch / ARCHIVE
        print(f"Downloading {ARCHIVE_URL}")
        try:
            actual = download(ARCHIVE_URL, archive_path)
        except OSError as error:
            raise FetchError(f"cannot download {ARCHIVE_URL}: {error}") from error
        if actual != ARCHIVE_SHA256:
            raise FetchError(f"{ARCHIVE}: SHA-256 is {actual}, pinned {ARCHIVE_SHA256}")

        staging = scratch / "ccache"
        staging.mkdir()
        try:
            unpack(archive_path, staging)
        except tarfile.TarError as error:
            raise FetchError(f"{ARCHIVE}: cannot unpack: {error}") from error
        (staging / STAMP).write_text(stamp_text(), encoding="utf-8")
        install(staging, prebuilt / DESTINATION.name, scratch)

    binary = DESTINATION / "ccache"
    version = reported_version(binary)
    if not version.startswith(f"ccache version {VERSION}"):
        raise FetchError(f"{binary} reports {version!r}, expected ccache version {VERSION}")
    print(f"{binary}: {version}")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--force", action="store_true",
                        help="download and unpack again even if the pinned archive is in place")
    args = parser.parse_args(argv)
    try:
        return fetch(args.force)
    except (FetchError, subprocess.CalledProcessError, OSError) as error:
        print(f"fetch_ccache.py: error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
