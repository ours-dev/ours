from __future__ import annotations

import argparse
import json
import os
import pathlib
import sys
import tempfile
from collections import defaultdict
from collections.abc import Sequence

if __package__ in (None, ""):
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))
    from tools.annotation.actions import ActionError, dispatch
    from tools.annotation.config import load_config, register_builtins
    from tools.annotation.discovery import DiscoveryError, discover_source, run_clang_ast_dump
    from tools.annotation.model import ActionRequest, ActionResult
    from tools.annotation.registry import Registry
else:
    from .actions import ActionError, dispatch
    from .config import load_config, register_builtins
    from .discovery import DiscoveryError, discover_source, run_clang_ast_dump
    from .model import ActionRequest, ActionResult
    from .registry import Registry


class CliError(ValueError):
    """Raised when annotation processing cannot produce a complete result."""


def _relative_source(source: pathlib.Path, source_root: pathlib.Path) -> pathlib.PurePosixPath:
    try:
        return pathlib.PurePosixPath(source.resolve().relative_to(source_root.resolve()).as_posix())
    except ValueError:
        raise CliError(f"source is outside source root: {source}")


def _validate_output_root(output_root: pathlib.Path, build_root: pathlib.Path) -> None:
    try:
        output_root.resolve().relative_to(build_root.resolve())
    except ValueError as error:
        raise CliError(f"output root must be inside build root: {output_root}") from error


def _write_atomic(path: pathlib.Path, contents: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", dir=path.parent, delete=False
    ) as temporary:
        temporary.write(contents)
        temporary_path = pathlib.Path(temporary.name)
    os.replace(temporary_path, path)


def _materialize(
    results: Sequence[ActionResult],
    output_root: pathlib.Path,
    manifest_path: pathlib.Path,
) -> None:
    outputs = sorted(
        {
            generated.path.as_posix()
            for result in results
            for generated in result.generated_files
        }
    )
    dependencies = sorted(
        {
            dependency.as_posix()
            for result in results
            for dependency in result.dependencies
        }
    )
    for result in results:
        for generated in result.generated_files:
            _write_atomic(
                output_root / pathlib.Path(generated.path.as_posix()),
                generated.contents,
            )
    manifest = {
        "outputs": outputs,
        "dependencies": dependencies,
        "diagnostics": [
            {
                "severity": diagnostic.severity,
                "message": diagnostic.message,
                "source_file": (
                    diagnostic.source_file.as_posix()
                    if diagnostic.source_file is not None
                    else None
                ),
                "line": diagnostic.line,
                "column": diagnostic.column,
            }
            for result in results
            for diagnostic in result.diagnostics
        ],
    }
    _write_atomic(manifest_path, json.dumps(manifest, sort_keys=True, indent=2) + "\n")


def _parse_args(argv: Sequence[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--flag", action="append", default=[])
    parser.add_argument("--source", action="append", required=True)
    parser.add_argument("--config", required=True, type=pathlib.Path)
    parser.add_argument("--source-root", type=pathlib.Path, default=pathlib.Path.cwd())
    parser.add_argument("--output-root", required=True, type=pathlib.Path)
    parser.add_argument("--build-root", required=True, type=pathlib.Path)
    parser.add_argument("--manifest", required=True, type=pathlib.Path)
    return parser.parse_args(argv)


def run(args: argparse.Namespace) -> None:
    registry = Registry()
    register_builtins(registry)
    load_config(args.config.resolve(), registry)
    _validate_output_root(args.output_root, args.build_root)
    records = []
    for source_name in args.source:
        source = pathlib.Path(source_name)
        ast = run_clang_ast_dump(args.compiler, args.flag, source)
        records.extend(
            discover_source(
                ast,
                source.read_text(encoding="utf-8"),
                _relative_source(source, args.source_root),
                registry,
            )
        )
    grouped: dict[str, list] = defaultdict(list)
    for record in records:
        grouped[record.canonical_name].append(record)
    results: list[ActionResult] = []
    source_root = pathlib.PurePosixPath(args.source_root.resolve().as_posix())
    for action_name in sorted(grouped):
        spec = registry.resolve(action_name)
        if spec is None:
            raise CliError(f"no action registered for {action_name}")
        request = ActionRequest(
            schema_version=spec.schema_version,
            action_name=action_name,
            source_root=source_root,
            annotations=tuple(grouped[action_name]),
        )
        results.append(dispatch(spec, request, args.output_root.resolve()))
    _materialize(results, args.output_root.resolve(), args.manifest.resolve())


def main(argv: Sequence[str] | None = None) -> int:
    try:
        run(_parse_args(argv))
    except (ActionError, CliError, DiscoveryError, OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
