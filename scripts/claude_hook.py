"""What the repo's Claude Code hooks have in common: reading the payload the
harness sends on stdin, and answering it.

A PostToolUse hook receives JSON describing the tool call that just finished.
Two ways of answering are used here, and they mean different things:

  context(text)  adds a note the model sees, for something it should know but
                 need not act on ("the formatter changed this file").
  block(reason)  tells the model something is wrong and must be fixed. The edit
                 has already happened; this does not undo it, it makes the model
                 deal with the problem before going on.

Neither stops a tool call. A hook that fails for its own reasons exits non-zero
without blocking, so a broken hook is visible but never wedges an edit.
"""

# macOS still ships Python 3.9 as python3; this keeps the annotations lazy.
from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Optional


def read_payload(stdin_text: str) -> dict:
    try:
        payload = json.loads(stdin_text or "{}")
    except ValueError:
        return {}
    return payload if isinstance(payload, dict) else {}


def edited_path(payload: dict) -> Optional[Path]:
    """The file a Write or Edit just changed, made absolute, or None."""
    tool_input = payload.get("tool_input") or {}
    response = payload.get("tool_response") or {}
    target = tool_input.get("file_path") or (response.get("filePath") if isinstance(response, dict) else None)
    if not target:
        return None
    path = Path(target)
    if not path.is_absolute():
        path = Path(payload.get("cwd") or os.getcwd()) / path
    return path


def context(text: str) -> str:
    return json.dumps({"hookSpecificOutput": {"hookEventName": "PostToolUse", "additionalContext": text}})


def block(reason: str) -> str:
    return json.dumps({"decision": "block", "reason": reason})
