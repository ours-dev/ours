#!/usr/bin/env python3
"""Download prebuilt artifacts declared by a YAML manifest."""

import argparse
import hashlib
import shutil
import sys
from pathlib import Path
from urllib.parse import urlparse
from urllib.request import Request, urlopen

try:
    import yaml
except ImportError as error:
    raise SystemExit(
        "PyYAML is required. Install it with: python3 -m pip install -r "
        "scripts/requirements.txt"
    ) from error


class ActivationError(Exception):
    """Raised when the manifest or an artifact cannot be activated."""


def _sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _load_artifacts(config_path):
    try:
        with config_path.open("r", encoding="utf-8") as stream:
            document = yaml.safe_load(stream) or {}
    except (OSError, yaml.YAMLError) as error:
        raise ActivationError(f"cannot read YAML config {config_path}: {error}") from error

    if not isinstance(document, dict) or not isinstance(document.get("artifacts"), list):
        raise ActivationError("config must contain an 'artifacts' list")

    artifacts = []
    for index, artifact in enumerate(document["artifacts"]):
        if not isinstance(artifact, dict):
            raise ActivationError(f"artifacts[{index}] must be a mapping")
        name = artifact.get("name")
        url = artifact.get("url")
        checksum = artifact.get("sha256")
        if not isinstance(name, str) or not name.strip():
            raise ActivationError(f"artifacts[{index}].name must be a non-empty string")
        if not isinstance(url, str) or not url.strip():
            raise ActivationError(f"artifacts[{index}].url must be a non-empty string")
        if urlparse(url).scheme not in {"file", "http", "https"}:
            raise ActivationError(f"artifacts[{index}].url must use file, http, or https")
        if checksum is not None:
            if not isinstance(checksum, str) or len(checksum) != 64:
                raise ActivationError(f"artifacts[{index}].sha256 must be a 64-character hex string")
            try:
                int(checksum, 16)
            except ValueError as error:
                raise ActivationError(f"artifacts[{index}].sha256 must be hexadecimal") from error
            checksum = checksum.lower()
        artifacts.append((name, url, checksum))
    return artifacts


def _destination(output_dir, name):
    root = output_dir.resolve()
    destination = (root / name).resolve()
    try:
        destination.relative_to(root)
    except ValueError as error:
        raise ActivationError(f"artifact path escapes output directory: {name}") from error
    return destination


def _download(url, partial_path):
    request = Request(url, headers={"User-Agent": "ours-prebuilt-activator/1"})
    with urlopen(request, timeout=60) as response, partial_path.open("wb") as output:
        shutil.copyfileobj(response, output, length=1024 * 1024)


def download_artifacts(config_path, output_dir):
    """Download artifacts and return the destination paths that changed."""
    config_path = Path(config_path).resolve()
    output_dir = Path(output_dir).resolve()
    artifacts = _load_artifacts(config_path)
    output_dir.mkdir(parents=True, exist_ok=True)
    changed = []

    for name, url, checksum in artifacts:
        destination = _destination(output_dir, name)
        if destination.is_file() and (checksum is None or _sha256(destination) == checksum):
            continue

        destination.parent.mkdir(parents=True, exist_ok=True)
        partial_path = destination.with_name(destination.name + ".part")
        try:
            _download(url, partial_path)
            if checksum is not None and _sha256(partial_path) != checksum:
                raise ActivationError(f"checksum mismatch for {name}")
            partial_path.replace(destination)
        except ActivationError:
            partial_path.unlink(missing_ok=True)
            raise
        except Exception as error:
            partial_path.unlink(missing_ok=True)
            raise ActivationError(f"failed to download {name}: {error}") from error
        changed.append(destination)
    return changed


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=Path("prebuilt.yml"))
    parser.add_argument("--output-dir", type=Path, default=Path("prebuilt"))
    args = parser.parse_args(argv)
    try:
        changed = download_artifacts(args.config, args.output_dir)
    except ActivationError as error:
        print(f"activate.py: error: {error}", file=sys.stderr)
        return 1
    for path in changed:
        print(f"Downloaded {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
