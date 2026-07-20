from __future__ import annotations

import json
import pathlib
import subprocess
from collections.abc import Iterable
from typing import Any

from .model import AnnotationInfo
from .registry import Registry


class DiscoveryError(ValueError):
    """Raised when a registered annotation cannot be normalized safely."""


_DECLARATION_KINDS = {
    "FunctionDecl": "function",
    "CXXMethodDecl": "function",
    "RecordDecl": "record",
    "CXXRecordDecl": "record",
    "EnumDecl": "enum",
    "NamespaceDecl": "namespace",
}


def _walk(node: object) -> Iterable[dict[str, Any]]:
    if isinstance(node, dict):
        yield node
        for child in node.get("inner", []):
            yield from _walk(child)
    elif isinstance(node, list):
        for child in node:
            yield from _walk(child)


def _is_attribute(node: dict[str, Any]) -> bool:
    kind = node.get("kind", "")
    return kind.endswith("Attr") or kind.endswith("Attribute")


def _attribute_name(node: dict[str, Any]) -> str | None:
    for key in ("canonicalName", "attributeName", "spelling", "name"):
        value = node.get(key)
        if isinstance(value, str) and value:
            return value
    return None


def _location(node: dict[str, Any]) -> tuple[int, int]:
    location = node.get("loc", {})
    return int(location.get("line", 0)), int(location.get("col", 0))


def _source_error(source_file: pathlib.PurePosixPath, node: dict[str, Any], message: str) -> DiscoveryError:
    line, column = _location(node)
    return DiscoveryError(f"{source_file}:{line}:{column}: {message}")


def _belongs_to_source(
    node: dict[str, Any],
    source_file: pathlib.PurePosixPath,
    source_identity: pathlib.Path | None,
) -> bool:
    location_file = node.get("loc", {}).get("file")
    if not isinstance(location_file, str):
        return True
    if source_identity is not None:
        try:
            return pathlib.Path(location_file).resolve() == source_identity.resolve()
        except OSError:
            return False
    return pathlib.PurePosixPath(location_file).as_posix() == source_file.as_posix()


def _arguments(node: dict[str, Any]) -> dict[str, object]:
    value = node.get("arguments", {})
    if isinstance(value, dict):
        return dict(value)
    if isinstance(value, list):
        return {str(index): item for index, item in enumerate(value)}
    if value in (None, ""):
        return {}
    raise DiscoveryError("annotation arguments must be an object or array")


def _registered_namespace(canonical_name: str, registry: Registry) -> bool:
    return any(
        canonical_name.startswith(namespace + "::")
        for namespace in {
            spec.canonical_name.rsplit("::", 1)[0]
            for spec in registry.items()
        }
    )


def discover_from_ast(
    ast: dict[str, Any],
    source_file: pathlib.PurePosixPath,
    registry: Registry,
    source_identity: pathlib.Path | None = None,
) -> list[AnnotationInfo]:
    records: list[AnnotationInfo] = []
    for declaration in _walk(ast):
        if not _belongs_to_source(declaration, source_file, source_identity):
            continue
        declaration_kind = _DECLARATION_KINDS.get(declaration.get("kind"))
        if declaration_kind is None:
            continue
        attributes = [
            child for child in declaration.get("inner", [])
            if isinstance(child, dict) and _is_attribute(child)
        ]
        for attribute in attributes:
            canonical_name = _attribute_name(attribute)
            if canonical_name is None:
                continue
            spec = registry.resolve(canonical_name)
            if spec is None:
                if _registered_namespace(canonical_name, registry):
                    raise _source_error(
                        source_file,
                        declaration,
                        f"unknown registered annotation {canonical_name}",
                    )
                continue
            if declaration_kind not in spec.declaration_kinds:
                raise _source_error(
                    source_file,
                    declaration,
                    f"annotation {canonical_name} does not support {declaration_kind}",
                )
            name = declaration.get("name")
            qualified_name = declaration.get("qualifiedName", name)
            if not isinstance(name, str) or not isinstance(qualified_name, str):
                raise _source_error(
                    source_file,
                    declaration,
                    f"annotation {canonical_name} requires declaration identity",
                )
            line, column = _location(declaration)
            records.append(
                AnnotationInfo(
                    schema_version=spec.schema_version,
                    canonical_name=canonical_name,
                    source_file=source_file,
                    line=line,
                    column=column,
                    declaration_kind=declaration_kind,
                    declaration_name=name,
                    qualified_name=qualified_name,
                    declaration_usr=declaration.get("usr"),
                    arguments=_arguments(attribute),
                )
            )
    return sorted(
        records,
        key=lambda record: (
            record.source_file.as_posix(),
            record.line,
            record.column,
            record.canonical_name,
            record.qualified_name,
        ),
    )


def _line_column(source_text: str, offset: int) -> tuple[int, int]:
    line = source_text.count("\n", 0, offset) + 1
    last_newline = source_text.rfind("\n", 0, offset)
    return line, offset - last_newline


def _mask_non_code(source_text: str) -> str:
    characters = list(source_text)
    state = "code"
    index = 0
    while index < len(characters):
        current = characters[index]
        following = characters[index + 1] if index + 1 < len(characters) else ""
        if state == "code" and current == "/" and following == "/":
            state = "line_comment"
            characters[index] = characters[index + 1] = " "
            index += 2
            continue
        if state == "code" and current == "/" and following == "*":
            state = "block_comment"
            characters[index] = characters[index + 1] = " "
            index += 2
            continue
        if state == "code" and current in {'"', "'"}:
            state = "string" if current == '"' else "character"
            characters[index] = " "
            index += 1
            continue
        if state == "line_comment":
            if current == "\n":
                state = "code"
            else:
                characters[index] = " "
            index += 1
            continue
        if state == "block_comment":
            if current == "*" and following == "/":
                characters[index] = characters[index + 1] = " "
                index += 2
                state = "code"
                continue
            if current != "\n":
                characters[index] = " "
            index += 1
            continue
        if state in {"string", "character"}:
            if current == "\\":
                characters[index] = " "
                if index + 1 < len(characters) and characters[index + 1] != "\n":
                    characters[index + 1] = " "
                    index += 2
                    continue
            elif (state == "string" and current == '"') or (
                state == "character" and current == "'"
            ):
                state = "code"
            elif current != "\n":
                characters[index] = " "
            index += 1
            continue
        index += 1
    return "".join(characters)


def _source_attributes(source_text: str) -> Iterable[tuple[str, int]]:
    offset = source_text.find("[[")
    while offset >= 0:
        closing = source_text.find("]]", offset + 2)
        if closing < 0:
            yield "", offset
            return
        body = source_text[offset + 2 : closing].strip()
        name_end = len(body)
        for delimiter in (" ", "\t", "\n", "("):
            position = body.find(delimiter)
            if position >= 0:
                name_end = min(name_end, position)
        yield body[:name_end], offset
        offset = source_text.find("[[", closing + 2)


def discover_source(
    ast: dict[str, Any],
    source_text: str,
    source_file: pathlib.PurePosixPath,
    registry: Registry,
    source_identity: pathlib.Path | None = None,
) -> list[AnnotationInfo]:
    records = discover_from_ast(ast, source_file, registry, source_identity)
    masked_source = _mask_non_code(source_text)
    namespaces = {
        spec.canonical_name.rsplit("::", 1)[0]
        for spec in registry.items()
    }
    source_counts: dict[str, int] = {}
    for canonical_name, offset in _source_attributes(masked_source):
        if not canonical_name:
            line, column = _line_column(source_text, offset)
            raise DiscoveryError(f"{source_file}:{line}:{column}: unterminated annotation")
        if not any(canonical_name.startswith(namespace + "::") for namespace in namespaces):
            continue
        line, column = _line_column(source_text, offset)
        if registry.resolve(canonical_name) is None:
            raise DiscoveryError(
                f"{source_file}:{line}:{column}: unknown registered annotation "
                f"{canonical_name}"
            )
        source_counts[canonical_name] = source_counts.get(canonical_name, 0) + 1
    record_counts: dict[str, int] = {}
    for record in records:
        record_counts[record.canonical_name] = record_counts.get(record.canonical_name, 0) + 1
    for canonical_name, count in source_counts.items():
        if record_counts.get(canonical_name, 0) < count:
            raise DiscoveryError(
                f"{source_file}: compiler did not preserve registered annotation "
                f"{canonical_name} ({record_counts.get(canonical_name, 0)} of {count})"
            )
    return records


def run_clang_ast_dump(
    compiler: str,
    flags: list[str],
    source_file: pathlib.Path,
) -> dict[str, Any]:
    command = [
        compiler,
        *flags,
        "-Xclang",
        "-ast-dump=json",
        "-fsyntax-only",
        str(source_file),
    ]
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        detail = result.stderr.strip() or "compiler returned a failure status"
        raise DiscoveryError(f"{source_file}: compiler AST dump failed: {detail}")
    try:
        ast = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise DiscoveryError(f"{source_file}: invalid Clang AST JSON: {error}") from error
    if not isinstance(ast, dict):
        raise DiscoveryError(f"{source_file}: Clang AST root must be an object")
    return ast
