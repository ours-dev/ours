from __future__ import annotations

import json
from pathlib import PurePosixPath
from typing import Any

from .model import (
    ActionRequest,
    ActionResult,
    AnnotationInfo,
    Diagnostic,
    GeneratedFile,
)


class ActionError(ValueError):
    """Raised when an action request or result violates the wire contract."""


def _path(value: object, field: str) -> PurePosixPath:
    if not isinstance(value, str) or not value:
        raise ActionError(f"{field} must be a non-empty path")
    path = PurePosixPath(value)
    if path.is_absolute() or ".." in path.parts:
        raise ActionError(f"{field} must be a relative path: {value}")
    return path


def _annotation_to_dict(annotation: AnnotationInfo) -> dict[str, object]:
    return {
        "schema_version": annotation.schema_version,
        "canonical_name": annotation.canonical_name,
        "source_file": annotation.source_file.as_posix(),
        "line": annotation.line,
        "column": annotation.column,
        "declaration_kind": annotation.declaration_kind,
        "declaration_name": annotation.declaration_name,
        "qualified_name": annotation.qualified_name,
        "declaration_usr": annotation.declaration_usr,
        "arguments": dict(annotation.arguments),
    }


def request_to_json(request: ActionRequest) -> str:
    payload = {
        "schema_version": request.schema_version,
        "action_name": request.action_name,
        "source_root": request.source_root.as_posix(),
        "annotations": [_annotation_to_dict(annotation) for annotation in request.annotations],
    }
    return json.dumps(payload, ensure_ascii=True, sort_keys=True, separators=(",", ":"))


def result_to_json(result: ActionResult) -> str:
    payload = {
        "schema_version": result.schema_version,
        "generated_files": [
            {"path": generated.path.as_posix(), "contents": generated.contents}
            for generated in result.generated_files
        ],
        "dependencies": [dependency.as_posix() for dependency in result.dependencies],
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
            for diagnostic in result.diagnostics
        ],
        "exit_status": result.exit_status,
    }
    return json.dumps(payload, ensure_ascii=True, sort_keys=True, separators=(",", ":"))


def result_from_json(encoded: str) -> ActionResult:
    try:
        payload = json.loads(encoded)
    except json.JSONDecodeError as error:
        raise ActionError(f"action returned invalid JSON: {error}") from error
    if not isinstance(payload, dict):
        raise ActionError("action result must be a JSON object")
    required = {"schema_version", "generated_files", "dependencies", "diagnostics", "exit_status"}
    if not required.issubset(payload):
        missing = ", ".join(sorted(required - payload.keys()))
        raise ActionError(f"action result is missing fields: {missing}")
    if not isinstance(payload["schema_version"], int) or payload["schema_version"] < 1:
        raise ActionError("action result schema_version must be a positive integer")
    if not isinstance(payload["generated_files"], list):
        raise ActionError("generated_files must be an array")
    generated_files = []
    for item in payload["generated_files"]:
        if not isinstance(item, dict) or not isinstance(item.get("contents"), str):
            raise ActionError("each generated file needs path and string contents")
        generated_files.append(
            GeneratedFile(_path(item.get("path"), "generated file path"), item["contents"])
        )
    if not isinstance(payload["dependencies"], list):
        raise ActionError("dependencies must be an array")
    dependencies = tuple(_path(item, "dependency path") for item in payload["dependencies"])
    if not isinstance(payload["diagnostics"], list):
        raise ActionError("diagnostics must be an array")
    diagnostics = []
    for item in payload["diagnostics"]:
        if not isinstance(item, dict) or not isinstance(item.get("message"), str):
            raise ActionError("each diagnostic needs a message")
        severity = item.get("severity")
        if severity not in {"info", "warning", "error"}:
            raise ActionError("diagnostic severity must be info, warning, or error")
        source_file = item.get("source_file")
        diagnostics.append(
            Diagnostic(
                severity=severity,
                message=item["message"],
                source_file=_path(source_file, "diagnostic source_file") if source_file else None,
                line=item.get("line"),
                column=item.get("column"),
            )
        )
    exit_status = payload["exit_status"]
    if not isinstance(exit_status, int) or exit_status < 0:
        raise ActionError("exit_status must be a non-negative integer")
    return ActionResult(
        schema_version=payload["schema_version"],
        generated_files=tuple(generated_files),
        dependencies=dependencies,
        diagnostics=tuple(diagnostics),
        exit_status=exit_status,
    )
