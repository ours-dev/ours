from __future__ import annotations

from collections.abc import Iterable

from .model import ActionSpec, RegistryError


class Registry:
    def __init__(self) -> None:
        self._actions: dict[str, ActionSpec] = {}

    def register(self, spec: ActionSpec) -> None:
        if spec.canonical_name in self._actions:
            raise RegistryError(
                f"action already registered: {spec.canonical_name}"
            )
        self._actions[spec.canonical_name] = spec

    def resolve(self, canonical_name: str) -> ActionSpec | None:
        return self._actions.get(canonical_name)

    def items(self) -> Iterable[ActionSpec]:
        return tuple(
            self._actions[name] for name in sorted(self._actions)
        )


__all__ = ["Registry", "RegistryError"]
