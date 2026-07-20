from __future__ import annotations

from dataclasses import dataclass
from pathlib import PurePosixPath
from typing import Callable, Mapping, Sequence


class RegistryError(ValueError):
    """Raised when an action registration is not a valid framework contract."""


@dataclass(frozen=True)
class AnnotationInfo:
    schema_version: int
    canonical_name: str
    source_file: PurePosixPath
    line: int
    column: int
    declaration_kind: str
    declaration_name: str
    qualified_name: str
    declaration_usr: str | None
    arguments: Mapping[str, object]


@dataclass(frozen=True)
class ActionRequest:
    schema_version: int
    action_name: str
    source_root: PurePosixPath
    annotations: tuple[AnnotationInfo, ...]


@dataclass(frozen=True)
class GeneratedFile:
    path: PurePosixPath
    contents: str


@dataclass(frozen=True)
class Diagnostic:
    severity: str
    message: str
    source_file: PurePosixPath | None = None
    line: int | None = None
    column: int | None = None


@dataclass(frozen=True)
class ActionResult:
    schema_version: int
    generated_files: tuple[GeneratedFile, ...]
    dependencies: tuple[PurePosixPath, ...]
    diagnostics: tuple[Diagnostic, ...]
    exit_status: int

    @classmethod
    def empty(cls, schema_version: int) -> ActionResult:
        return cls(schema_version, (), (), (), 0)


ActionCallback = Callable[[ActionRequest], ActionResult]


@dataclass(frozen=True)
class ActionSpec:
    canonical_name: str
    declaration_kinds: tuple[str, ...]
    schema_version: int
    callback: ActionCallback | None = None
    command: tuple[str, ...] | None = None
    declared_outputs: tuple[PurePosixPath, ...] = ()

    def __post_init__(self) -> None:
        if not self.canonical_name or any(
            not part for part in self.canonical_name.split("::")
        ) or "::" not in self.canonical_name:
            raise RegistryError("canonical action name must contain a namespace")
        if not self.declaration_kinds or any(
            not declaration_kind for declaration_kind in self.declaration_kinds
        ):
            raise RegistryError("at least one declaration kind is required")
        if self.schema_version < 1:
            raise RegistryError("schema version must be positive")
        if (self.callback is None) == (self.command is None):
            raise RegistryError("provide exactly one callback or command")
        if self.command is not None and not self.command:
            raise RegistryError("external action command must not be empty")
        if any(path.is_absolute() or ".." in path.parts for path in self.declared_outputs):
            raise RegistryError("declared outputs must be relative paths")
        if len(set(self.declared_outputs)) != len(self.declared_outputs):
            raise RegistryError("declared outputs must be unique")


def normalize_command(command: Sequence[str]) -> tuple[str, ...]:
    return tuple(command)
