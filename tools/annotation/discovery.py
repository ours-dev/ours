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
) -> list[AnnotationInfo]:
    records: list[AnnotationInfo] = []
    for declaration in _walk(ast):
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
