"""Where a boot script keeps a build directory's logs and ESP images: a
directory of that build directory's own, under the per-user temp directory.

The temp directory is one per user, so every session and every worktree on the
machine shares it, and every worktree builds into a directory of the same name
(out/bringup.x86). A directory named for that name alone would be one directory
for all of them: two runs at once would write into each other's logs, and one
could remove and rebuild the ESP image the other's QEMU was about to open. So
the name also carries a digest of the build directory's whole path, which
differs between worktrees and stays the same from run to run: a person or a
document can still say where the last run's log is.

Used by the `ours-verify` skill's boot_check.py and riscv_boot.py, and by
scripts/kshell_run.py, scripts/cov_kernel.py and scripts/qemu_boot.py.
"""
from __future__ import annotations

import hashlib
import tempfile
from pathlib import Path


def path(prefix: str, out: Path) -> Path:
    """`<temp dir>/<prefix>-<out's name>-<10 hex digits>`, the digits the start
    of the SHA-1 of `out`'s resolved path.

    `out` is resolved here rather than by each caller, so that one build
    directory named relative by one script and absolute by another gets one
    directory. Nothing is created: the caller makes it when it writes."""
    out = Path(out).resolve()
    digest = hashlib.sha1(str(out).encode()).hexdigest()[:10]
    return Path(tempfile.gettempdir()) / f"{prefix}-{out.name}-{digest}"
