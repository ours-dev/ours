from __future__ import annotations

import pathlib
import subprocess

from .model import ActionRequest, ActionResult, ActionSpec
from .protocol import ActionError, request_to_json, result_from_json, result_to_json


def _validate_output_root(result: ActionResult, output_root: pathlib.Path) -> ActionResult:
    root = output_root.resolve()
    for generated in result.generated_files:
        candidate = (root / pathlib.Path(generated.path.as_posix())).resolve()
        try:
            candidate.relative_to(root)
        except ValueError as error:
            raise ActionError(f"generated file escapes output root: {generated.path}") from error
    paths = [generated.path for generated in result.generated_files]
    if len(paths) != len(set(paths)):
        raise ActionError("action returned duplicate generated file paths")
    return result


def run_external(command: tuple[str, ...], request: ActionRequest) -> ActionResult:
    result = subprocess.run(
        list(command),
        input=request_to_json(request),
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or "external action failed"
        raise ActionError(f"external action exited with {result.returncode}: {detail}")
    return result_from_json(result.stdout)


def dispatch(
    spec: ActionSpec,
    request: ActionRequest,
    output_root: pathlib.Path,
) -> ActionResult:
    if spec.callback is not None:
        result = spec.callback(request)
        if not isinstance(result, ActionResult):
            raise ActionError("in-process action did not return ActionResult")
        result = result_from_json(result_to_json(result))
    else:
        assert spec.command is not None
        result = run_external(spec.command, request)
    if result.schema_version != spec.schema_version:
        raise ActionError(
            f"action schema mismatch: expected {spec.schema_version}, got {result.schema_version}"
        )
    if result.exit_status != 0:
        raise ActionError(f"action returned failure status {result.exit_status}")
    if spec.declared_outputs:
        undeclared = {
            generated.path for generated in result.generated_files
        } - set(spec.declared_outputs)
        if undeclared:
            raise ActionError(
                "action returned undeclared generated files: "
                + ", ".join(sorted(path.as_posix() for path in undeclared))
            )
    return _validate_output_root(result, output_root)


__all__ = ["ActionError", "dispatch", "run_external"]
