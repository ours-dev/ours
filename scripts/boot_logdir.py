"""Where a boot script keeps one build directory's logs and ESP images.

Shared by scripts/kshell_run.py and the ours-verify skill's boot_check.py, so
the two cannot come to name it differently.
"""

from __future__ import annotations

import hashlib
import tempfile
from pathlib import Path


def log_directory(prefix: str, out: Path) -> Path:
    """The directory for |prefix|'s runs against the build directory |out|.

    Named after the whole path of |out|, not only its last component. Every
    worktree builds into out/bringup.x86, so a name made from the last
    component alone is one directory for every worktree on the machine. It
    holds the serial log, the monitor log and the ESP image QEMU boots from.
    A second run deletes and rebuilds the image directory, and opens the log
    for writing, which truncates it while the first run's script is still
    writing to it at its own offset: on 2026-10-26 a kshell_run.py log held
    another session's commands with its timestamps running backwards.

    The last component stays in the name so that a listing of $TMPDIR still
    says which build a directory is for. |out| should already be resolved, so
    that two spellings of one path name one directory.
    """
    tag = hashlib.sha256(str(out).encode()).hexdigest()[:10]
    return Path(tempfile.gettempdir()) / f"{prefix}-{out.name}-{tag}"
