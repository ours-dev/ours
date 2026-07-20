from __future__ import annotations

import json
import pathlib
from typing import Any

from .model import ActionSpec
from .registry import Registry


def register_builtins(registry: Registry) -> None:
    """Hook for in-process framework actions; V1 has no built-in semantics."""


def _resolve_command(command: list[str], config_path: pathlib.Path) -> tuple[str, ...]:
    resolved: list[str] = []
    for index, token in enumerate(command):
        if index > 0 and ("/" in token or token.startswith(".")):
            path = pathlib.Path(token)
            if not path.is_absolute():
                path = (config_path.parent / path).resolve()
            resolved.append(str(path))
        else:
            resolved.append(token)
    return tuple(resolved)


def load_config(config_path: pathlib.Path, registry: Registry) -> None:
    payload: Any = json.loads(config_path.read_text(encoding="utf-8"))
    actions = payload.get("actions") if isinstance(payload, dict) else None
    if not isinstance(actions, list):
        raise ValueError("annotation config must contain an actions array")
    for action in actions:
        if not isinstance(action, dict):
            raise ValueError("each annotation action registration must be an object")
        command = action.get("command")
        if not isinstance(command, list) or not all(isinstance(item, str) for item in command):
            raise ValueError("external action command must be a string array")
        declaration_kinds = action.get("declaration_kinds")
        if not isinstance(declaration_kinds, list) or not all(
            isinstance(item, str) for item in declaration_kinds
        ):
            raise ValueError("declaration_kinds must be a string array")
        schema_version = action.get("schema_version")
        if not isinstance(schema_version, int):
            raise ValueError("schema_version must be an integer")
        registry.register(
            ActionSpec(
                canonical_name=action.get("attribute", ""),
                declaration_kinds=tuple(declaration_kinds),
                schema_version=schema_version,
                command=_resolve_command(command, config_path),
            )
        )
