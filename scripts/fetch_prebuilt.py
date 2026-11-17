#!/usr/bin/env python3
"""Fill prebuilt/ with the toolchain pinned in build/prebuilt.json.

    python3 scripts/fetch_prebuilt.py [--dest DIR] [--only NAME ...] [--check]
                                      [--force] [--downloads DIR]

The build and the boot scripts need a compiler, gn, ninja, ccache, rustc,
QEMU and the GRUB tools under prebuilt/. That tree is not in git, so a machine
that has never had it -- a CI runner above all -- gets it here, from upstream
releases pinned by SHA-256, and ends with the same versions any other machine
has.

Each entry of the manifest's "tools" is one directory under prebuilt/
("dest"), built from one or more archives ("sources") plus a few small files
written verbatim ("files"). A source is one of:

  - an archive by URL: "url", "sha256", "format" (tar.xz, tar.gz or zip) and
    "strip", the leading path components to drop;
  - a Homebrew bottle: "bottle" (the formula), "tag" (the ghcr.io tag, the
    formula's version with its revision and rebuild), "platform" and "sha256"
    (the bottle's blob digest). The blob is fetched from ghcr.io with an
    anonymous token, and its leading <formula>/<version>/ is dropped. Bottles
    are not relocated: QEMU and GRUB find their libraries through
    DYLD_LIBRARY_PATH, set by scripts/qemu_boot.py, which searches by a
    library's file name whatever directory its load command names.

"keep" lists path prefixes, after "strip"; only members under one of them are
unpacked, and each must match something, so a typo or an upstream layout
change fails here rather than in the build. "rename" maps a prefix to another
one ("" drops it), applied after "keep".

A finished directory carries STAMP, the canonical JSON of its manifest entry.
A directory whose stamp equals its entry is left alone; one whose stamp
differs was put there by this script and is replaced. A directory without a
stamp was put there some other way (by hand, before this script) and is never
replaced without --force, so running this on a machine with a hand-built
tree changes nothing of it.

A new directory is built in a scratch directory beside the destination and
renamed into place only after every source matched its hash and the tool's
"check" command reported the pinned version, so an interrupted run leaves the
old tree as it was.

To pin a new bottle, read the tag list and the image index of the formula at
https://ghcr.io/v2/homebrew/core/<formula>/ (tags/list, manifests/<tag>, with
a token from https://ghcr.io/token?scope=repository:homebrew/core/<formula>:pull)
and take the "sh.brew.bottle.digest" annotation of the oldest macOS platform
the CI runner can run.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import zipfile
from pathlib import Path, PurePosixPath
from typing import Iterator, Optional
from urllib.error import HTTPError
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "build" / "prebuilt.json"

# Written last into a finished directory: what it was built from.
STAMP = ".prebuilt.json"

USER_AGENT = "ours-fetch-prebuilt/1"
GHCR = "https://ghcr.io"

# Tries per archive, each continuing where the last one stopped.
ATTEMPTS = 20


class FetchError(Exception):
    """A source could not be downloaded, did not match its pin, or did not
    unpack into what the manifest describes."""


def host() -> str:
    machine = platform.machine()
    cpu = {"arm64": "arm64", "aarch64": "arm64", "x86_64": "x64"}.get(machine, machine)
    system = {"Darwin": "mac", "Linux": "linux"}.get(platform.system(), platform.system())
    return f"{system}-{cpu}"


def stamp_text(entry: dict) -> str:
    return json.dumps(entry, sort_keys=True, indent=1) + "\n"


def state(directory: Path, entry: dict) -> str:
    """'current', 'stale' (ours, from another pin), 'foreign' (not ours) or 'missing'."""
    if not directory.exists() and not directory.is_symlink():
        return "missing"
    try:
        text = (directory / STAMP).read_text(encoding="utf-8")
    except OSError:
        return "foreign"
    return "current" if text == stamp_text(entry) else "stale"


# Downloading.

def ghcr_token(formula: str) -> str:
    repository = ghcr_repository(formula)
    url = f"{GHCR}/token?scope=repository:homebrew/core/{repository}:pull"
    with urlopen(Request(url, headers={"User-Agent": USER_AGENT}), timeout=60) as response:
        return json.load(response)["token"]


def ghcr_repository(formula: str) -> str:
    # Homebrew's own mapping of a formula name to its package name on ghcr.io.
    return formula.replace("@", "/").replace("+", "x")


def source_url(source: dict) -> str:
    if "bottle" in source:
        return (f"{GHCR}/v2/homebrew/core/{ghcr_repository(source['bottle'])}"
                f"/blobs/sha256:{source['sha256']}")
    return source["url"]


def source_name(source: dict) -> str:
    if "bottle" in source:
        return f"{source['bottle']} {source['tag']} {source['platform']}"
    return source["url"].rsplit("/", 1)[-1]


def open_url(source: dict, offset: int):
    """The response for `source`'s archive, from byte `offset` on."""
    headers = {"User-Agent": USER_AGENT}
    if offset:
        headers["Range"] = f"bytes={offset}-"
    if "bottle" in source:
        # ghcr.io answers a blob with a redirect to signed storage, and the
        # token must not follow it there; urllib would forward it, so the
        # redirect is taken by hand.
        request = Request(source_url(source), headers={
            **headers, "Authorization": f"Bearer {ghcr_token(source['bottle'])}"})
        response = _no_redirect_opener().open(request, timeout=60)
        location = response.headers.get("Location") if response.status in (301, 302, 303, 307, 308) else None
        if location is None:
            return response
        response.close()
        return urlopen(Request(location, headers=headers), timeout=60)
    return urlopen(Request(source_url(source), headers=headers), timeout=60)


def _no_redirect_opener():
    import urllib.request

    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, *args, **kwargs):
            return None

        def http_error_302(self, req, fp, code, msg, headers):
            return fp

        http_error_301 = http_error_303 = http_error_307 = http_error_308 = http_error_302

    return urllib.request.build_opener(NoRedirect)


def download(source: dict, downloads: Path) -> Path:
    """The archive of `source`, from `downloads` if an earlier run saved it
    there under its hash, otherwise fetched and checked first."""
    path = downloads / source["sha256"]
    if path.is_file() and file_sha256(path) == source["sha256"]:
        return path
    partial = path.with_name(path.name + ".part")
    print(f"  downloading {source_name(source)}", flush=True)
    error: Optional[Exception] = None
    for attempt in range(ATTEMPTS):
        if attempt:
            time.sleep(min(2 ** attempt, 30))
        try:
            if fetch_rest(source, partial):
                break
            error = FetchError("the connection closed before the end")
        except OSError as caught:
            error = caught
        print(f"  retrying {source_name(source)} after {error}", flush=True)
    else:
        raise FetchError(f"cannot download {source_url(source)}: {error}")
    actual = file_sha256(partial)
    if actual != source["sha256"]:
        partial.unlink()
        raise FetchError(f"{source_name(source)}: SHA-256 is {actual}, pinned {source['sha256']}")
    partial.rename(path)
    return path


def fetch_rest(source: dict, partial: Path) -> bool:
    """Appends what `partial` lacks of the archive; True once it is whole.

    A connection can end early without any error -- an HTTP proxy cutting a
    long transfer looks exactly like the end of the file -- so the length the
    server announced is compared with what arrived, and a short file is
    continued with a range request rather than started over."""
    offset = partial.stat().st_size if partial.exists() else 0
    try:
        response = open_url(source, offset)
    except HTTPError as error:
        if error.code == 416 and offset:
            return True  # the range starts at the end: nothing is missing
        raise
    with response:
        if offset and response.status != 206:
            offset = 0  # the server ignored the range and sent it all
        length = response.headers.get("Content-Length")
        expected = offset + int(length) if length is not None else None
        with partial.open("ab" if offset else "wb") as output:
            shutil.copyfileobj(response, output, 1024 * 1024)
    return expected is None or partial.stat().st_size >= expected


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


# Unpacking.

class Member:
    """One archive entry, whatever the archive format."""

    def __init__(self, name: str, kind: str, mode: int, link: str = "", data=None):
        self.name = name      # path inside the archive, '/'-separated
        self.kind = kind      # 'file', 'dir', 'symlink' or 'hardlink'
        self.mode = mode
        self.link = link      # a link's target, as the archive records it
        self.data = data      # a callable returning a readable stream, for files


def tar_members(path: Path) -> Iterator[Member]:
    # Streaming mode: an xz archive cannot seek, and a 1.5 GB one is read once.
    with tarfile.open(path, "r|*") as archive:
        for info in archive:
            if info.isdir():
                kind = "dir"
            elif info.isfile():
                kind = "file"
            elif info.issym():
                kind = "symlink"
            elif info.islnk():
                kind = "hardlink"
            else:
                raise FetchError(f"{path.name}: {info.name!r} is not a file, directory or link")
            yield Member(info.name, kind, info.mode, info.linkname,
                         (lambda info=info: archive.extractfile(info)))


def zip_members(path: Path) -> Iterator[Member]:
    with zipfile.ZipFile(path) as archive:
        for info in archive.infolist():
            mode = (info.external_attr >> 16) & 0o777 or 0o644
            kind = "dir" if info.is_dir() else "file"
            yield Member(info.filename, kind, mode, "",
                         (lambda info=info: archive.open(info)))


def relative(name: str, strip: int) -> Optional[PurePosixPath]:
    """`name` with its first `strip` components dropped, None if nothing is left.
    Refuses anything that could land outside the destination."""
    path = PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts:
        raise FetchError(f"unsafe path in archive: {name!r}")
    parts = [part for part in path.parts if part != "."]
    if len(parts) <= strip:
        return None
    return PurePosixPath(*parts[strip:])


def under(path: PurePosixPath, prefix: str) -> bool:
    prefix_path = PurePosixPath(prefix)
    return path == prefix_path or prefix_path in path.parents


def renamed(path: PurePosixPath, rename: dict) -> Optional[PurePosixPath]:
    for old, new in rename.items():
        if under(path, old):
            rest = path.relative_to(old)
            result = PurePosixPath(new) / rest if new else rest
            return None if str(result) == "." else result
    return path


def unpack(archive: Path, source: dict, staging: Path) -> None:
    strip = source.get("strip", 2 if "bottle" in source else 0)
    keep = source.get("keep")
    rename = source.get("rename", {})
    matched = set()
    # Where each kept member landed, for hard links that name an earlier one.
    placed: dict[str, Path] = {}
    members = zip_members(archive) if source.get("format") == "zip" else tar_members(archive)
    for member in members:
        path = relative(member.name, strip)
        if path is None:
            continue
        if keep is not None:
            hits = [prefix for prefix in keep if under(path, prefix)]
            if not hits:
                continue
            matched.update(hits)
        path = renamed(path, rename)
        if path is None:
            continue
        target = staging / path
        if member.kind == "dir":
            target.mkdir(parents=True, exist_ok=True)
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.exists() or target.is_symlink():
            raise FetchError(f"{source_name(source)}: {path} is already there from another source")
        if member.kind == "file":
            with member.data() as stream, target.open("wb") as output:
                shutil.copyfileobj(stream, output, 1024 * 1024)
            # Executable for everyone or for no one: archives disagree on
            # group and other bits, and nothing here is private.
            target.chmod(0o755 if member.mode & 0o100 else 0o644)
        elif member.kind == "symlink":
            landing = PurePosixPath(os.path.normpath(str(path.parent / member.link)))
            if PurePosixPath(member.link).is_absolute() or landing.parts[:1] == ("..",):
                raise FetchError(f"{source_name(source)}: {path} links outside the tree "
                                 f"({member.link})")
            os.symlink(member.link, target)
        else:
            original = placed.get(member.link)
            if original is None:
                raise FetchError(f"{source_name(source)}: {path} is a hard link to "
                                 f"{member.link}, which is not kept")
            os.link(original, target)
        placed[member.name] = target
    if keep is not None:
        missing = [prefix for prefix in keep if prefix not in matched]
        if missing:
            raise FetchError(f"{source_name(source)}: nothing in the archive under "
                             f"{', '.join(missing)}")


# Checking and installing.

def run_check(directory: Path, entry: dict) -> None:
    check = entry.get("check")
    if check is None:
        return
    command = [str(directory / check["command"][0]), *check["command"][1:]]
    env = dict(os.environ)
    if "library_path" in check:
        env["DYLD_LIBRARY_PATH"] = str(directory / check["library_path"])
    try:
        result = subprocess.run(command, capture_output=True, text=True, env=env, timeout=120)
    except OSError as error:
        raise FetchError(f"cannot run {' '.join(command)}: {error}") from error
    output = result.stdout + result.stderr
    if check["expect"] not in output:
        first = output.strip().splitlines()[:3]
        raise FetchError(f"{' '.join(check['command'])} printed {first!r}, "
                         f"expected {check['expect']!r}")


def build(name: str, entry: dict, destination: Path, downloads: Path) -> None:
    parent = destination.parent
    parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=f".fetch-{name}-", dir=parent) as scratch_name:
        scratch = Path(scratch_name)
        staging = scratch / "tree"
        staging.mkdir()
        for source in entry.get("sources", []):
            archive = download(source, downloads)
            print(f"  unpacking {source_name(source)}", flush=True)
            try:
                unpack(archive, source, staging)
            except (tarfile.TarError, zipfile.BadZipFile, EOFError) as error:
                raise FetchError(f"{source_name(source)}: cannot unpack: {error}") from error
        dangling = [str(path.relative_to(staging)) for path in staging.rglob("*")
                    if path.is_symlink() and not path.exists()]
        if dangling:
            raise FetchError(f"{name}: links to something not kept: {', '.join(sorted(dangling))}")
        for relative_path, text in entry.get("files", {}).items():
            path = staging / relative_path
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        run_check(staging, entry)
        (staging / STAMP).write_text(stamp_text(entry), encoding="utf-8")
        if destination.is_symlink() or destination.is_file():
            destination.unlink()
        elif destination.exists():
            destination.rename(scratch / "previous")
        staging.rename(destination)


def load_manifest(path: Path) -> dict:
    manifest = json.loads(path.read_text(encoding="utf-8"))
    if manifest.get("host") != host():
        raise FetchError(f"{path} pins tools for {manifest.get('host')}; this host is {host()}")
    return manifest


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--dest", type=Path, default=ROOT / "prebuilt",
                        help="the prebuilt directory to fill (default: prebuilt/)")
    parser.add_argument("--only", nargs="+", metavar="NAME", help="these tools only")
    parser.add_argument("--check", action="store_true",
                        help="report what is missing or stale, change nothing")
    parser.add_argument("--force", action="store_true",
                        help="rebuild even a current tree, and replace one without a stamp")
    parser.add_argument("--downloads", type=Path,
                        help="keep archives here by hash and reuse them (default: discard)")
    parser.add_argument("--manifest", type=Path, default=MANIFEST, help=argparse.SUPPRESS)
    args = parser.parse_args(argv)

    try:
        manifest = load_manifest(args.manifest)
        tools = manifest["tools"]
        names = args.only or list(tools)
        unknown = [name for name in names if name not in tools]
        if unknown:
            raise FetchError(f"not in the manifest: {', '.join(unknown)}")

        problems = 0
        with tempfile.TemporaryDirectory(prefix="ours-prebuilt-") as temporary:
            downloads = args.downloads or Path(temporary)
            downloads.mkdir(parents=True, exist_ok=True)
            for name in names:
                entry = tools[name]
                destination = args.dest / entry["dest"]
                current = state(destination, entry)
                if current == "current" and not args.force:
                    print(f"{name}: current")
                    continue
                if current == "foreign" and not args.force:
                    print(f"{name}: {destination} was not made by this script; left alone "
                          f"(--force replaces it)")
                    continue
                if args.check:
                    print(f"{name}: {current}")
                    problems += 1
                    continue
                print(f"{name}: {current}, fetching", flush=True)
                build(name, entry, destination, downloads)
                print(f"{name}: installed in {destination}")
        return 1 if problems else 0
    except (FetchError, OSError, json.JSONDecodeError, KeyError) as error:
        print(f"fetch_prebuilt.py: error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
