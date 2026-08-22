#!/usr/bin/env python3
# Copyright 2026 The Ours Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Wrap a command so unchanged outputs keep their old mtime.

Usage:
    restat_cacher.py --outputs OUT... -- COMMAND [ARGS...]

How it works:
  1. Every declared output that already exists is moved aside to a backup
     name (sibling file, ".restat-backup" suffix).
  2. The wrapped command runs unmodified, writing fresh outputs.
  3. On success, each new output is compared byte-for-byte with its backup:
       - identical: the backup is moved back over the new output, restoring
         the OLD mtime.  Ninja (with `restat` on the tool) then treats the
         output as up-to-date and may prune the action graph.
       - different: the backup is deleted, the new output (fresh mtime)
         stays.
  4. On command failure, backups are restored and the command exit code
     is propagated.

This is the lean local port of Fuchsia //build/tracer/restat_cacher.py
(Copyright 2021 The Fuchsia Authors, BSD-style license).  Fuchsia also
supports RBE remote-execution modes and a --temp-dir/--temp-prefix knob;
the Ours build is local-only, so only the default behavior is ported.
"""

import argparse
import filecmp
import os
import shutil
import subprocess
import sys

_BACKUP_SUFFIX = ".restat-backup"


def _backup_existing(outputs):
    """Move pre-existing outputs aside.  Returns {output -> backup_path}."""
    backups = {}
    for output in outputs:
        if os.path.exists(output):
            backup = output + _BACKUP_SUFFIX
            # A stale backup from a previous interrupted run would make the
            # move below fail; remove it first.
            if os.path.exists(backup):
                _remove(backup)
            shutil.move(output, backup)
            backups[output] = backup
    return backups


def _restore_all(backups):
    """Move every backup back to its original path (command failed)."""
    for output, backup in backups.items():
        shutil.move(backup, output)


def _restore_if_unchanged(backups):
    """For each backup: if it matches the new output, restore the old mtime
    by moving the backup back; otherwise drop the backup (new output wins).
    Returns True if any move failed."""
    error = False
    for output, backup in backups.items():
        try:
            if filecmp.cmp(backup, output, shallow=False):
                # Byte-identical: restore the backup so the old mtime
                # (and thus Ninja restat) is preserved.
                shutil.move(backup, output)
            else:
                # Content changed: the fresh output stands, drop the backup.
                _remove(backup)
        except FileNotFoundError as exc:
            print(exc, file=sys.stderr)
            error = True
    return error


def _remove(path):
    """Remove a file or (rarely) directory."""
    if os.path.isdir(path) and not os.path.islink(path):
        shutil.rmtree(path)
    else:
        os.unlink(path)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--outputs", nargs="*", default=[],
                        help="Declared output files of the wrapped action")
    parser.add_argument("--disable", action="store_false", dest="enable",
                        default=True,
                        help="Run the command as-is (no wrapping)")
    parser.add_argument("command", nargs="*", help="The command to run")
    args = parser.parse_args(argv)

    if not args.enable or not args.command:
        # Nothing to wrap: hand the command through verbatim.
        return subprocess.call(args.command)

    backups = _backup_existing(args.outputs)
    retval = subprocess.call(args.command)
    if retval != 0:
        # Restore the pre-command state so the outputs are not left half-
        # written; the build will retry the failing action next time.
        _restore_all(backups)
        return retval

    if _restore_if_unchanged(backups):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
