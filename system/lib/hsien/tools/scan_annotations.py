#!/usr/bin/env python3

from __future__ import annotations

import argparse
import dataclasses
import json
import pathlib
import re
import subprocess
import sys
from typing import Any, Iterable


class AnnotationError(ValueError):
    pass


@dataclasses.dataclass(frozen=True)
class TestRecord:
    name: str
    qualified_name: str
    source_path: pathlib.PurePath
    line: int
    column: int


_CONTEXT_SIGNATURE = re.compile(r"void\s*\(\s*hsien::Context\s*&\s*\)$")


def _walk(value: Any) -> Iterable[dict[str, Any]]:
    if isinstance(value, dict):
        yield value
        for child in value.get("inner", []):
            yield from _walk(child)
    elif isinstance(value, list):
        for child in value:
            yield from _walk(child)


def _location(node: dict[str, Any], source_path: pathlib.PurePath) -> tuple[int, int]:
    location = node.get("loc", {})
    return int(location.get("line", 0)), int(location.get("col", 0))


def _annotation_type(annotation: dict[str, Any]) -> str | None:
    for node in _walk(annotation.get("inner", [])):
        if node.get("kind") != "CXXConstructExpr":
            continue
        type_info = node.get("type", {})
        return type_info.get("desugaredQualType") or type_info.get("qualType")
    return None


def _is_hsien_test(annotation: dict[str, Any]) -> bool:
    if _annotation_type(annotation) not in {"hsien::test_t", "test_t"}:
        return False
    for node in _walk(annotation.get("inner", [])):
        if node.get("kind") != "DeclRefExpr":
            continue
        referenced = node.get("referencedDecl", {})
        if referenced.get("name") != "test":
            continue
        type_info = referenced.get("type", {})
        referenced_type = type_info.get("desugaredQualType", "")
        return referenced_type.endswith("hsien::test_t")
    return False


def scan_ast(ast: dict[str, Any], source_path: pathlib.PurePath) -> list[TestRecord]:
    records: list[TestRecord] = []
    for function in _walk(ast):
        if function.get("kind") != "FunctionDecl":
            continue
        annotations = [
            child
            for child in function.get("inner", [])
            if child.get("kind") == "CXX26AnnotationAttr"
        ]
        if not annotations:
            continue
        if len(annotations) != 1 or not _is_hsien_test(annotations[0]):
            line, column = _location(function, source_path)
            raise AnnotationError(
                f"{source_path}:{line}:{column}: unsupported hsien annotation"
            )

        type_name = function.get("type", {}).get("qualType", "")
        if _CONTEXT_SIGNATURE.fullmatch(type_name) is None:
            line, column = _location(function, source_path)
            raise AnnotationError(
                f"{source_path}:{line}:{column}: hsien test must have signature "
                "void(hsien::Context&)"
            )

        line, column = _location(function, source_path)
        records.append(
            TestRecord(
                name=function["name"],
                qualified_name=function.get("qualifiedName", function["name"]),
                source_path=source_path,
                line=line,
                column=column,
            )
        )
    return records


def sort_records(records: Iterable[TestRecord]) -> list[TestRecord]:
    return sorted(records, key=lambda record: (record.source_path.as_posix(), record.name))


def _cpp_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=True)


def render_wrapper(
    source_path: pathlib.PurePath,
    records: Iterable[TestRecord],
    include_root: pathlib.PurePath | None = None,
    record_offset: int = 0,
) -> str:
    include_path = source_path
    if include_root is not None:
        include_path = pathlib.PurePath(source_path).relative_to(include_root)

    lines = [
        "#include <hsien/test.hpp>",
        f"#include {_cpp_string(include_path.as_posix())}",
        "",
    ]
    for index, record in enumerate(records, start=record_offset):
        lines.extend(
            [
                "namespace {",
                "__attribute__((section(\".hsien.tests\"), used))",
                f"constinit hsien::TestRecord hsien_record_{index} {{",
                f"    .name = {_cpp_string(record.name)},",
                f"    .invoke = &{record.qualified_name},",
                "    .location = std::source_location::current(),",
                "};",
                "} // namespace",
                "",
            ]
        )
    return "\n".join(lines)


def _run_ast_dump(
    compiler: str, flags: list[str], source_path: pathlib.Path
) -> dict[str, Any]:
    command = [
        compiler,
        *flags,
        "-Xclang",
        "-ast-dump=json",
        "-fsyntax-only",
        str(source_path),
    ]
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise AnnotationError(result.stderr.strip() or f"compiler failed: {command}")
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise AnnotationError(f"Clang did not emit AST JSON for {source_path}: {error}") from error


def _parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--flag", action="append", default=[])
    parser.add_argument("--include-root", type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("sources", nargs="+", type=pathlib.Path)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(sys.argv[1:] if argv is None else argv)
    try:
        records: list[TestRecord] = []
        for source_path in args.sources:
            ast = _run_ast_dump(args.compiler, args.flag, source_path)
            records.extend(scan_ast(ast, source_path))
        ordered = sort_records(records)
        grouped: dict[pathlib.PurePath, list[TestRecord]] = {}
        for record in ordered:
            grouped.setdefault(record.source_path, []).append(record)
        output = []
        record_offset = 0
        for source_path, source_records in grouped.items():
            output.append(
                render_wrapper(
                    source_path,
                    source_records,
                    args.include_root,
                    record_offset,
                )
            )
            record_offset += len(source_records)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text("\n".join(output), encoding="utf-8")
        return 0
    except (AnnotationError, OSError, subprocess.SubprocessError) as error:
        print(error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
