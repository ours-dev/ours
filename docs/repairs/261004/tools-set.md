# tools-set outstanding repairs

## 1. set.py's --help still explains itself by another build's command and boot option

**Severity**
Low: help text only; every option works as documented.

**Symptom**
`python3 tools/set.py --help` prints the description "Configure an Ours build directory (fx set equivalent)." and, for `--autorun`, "(ours.autorun.boot, like zircon.autorun.boot)". A reader who does not know the other build learns nothing from either comparison, and the module docstring, which the comment sweep of 2026-10-04 rewrote, already says what both do in this tree's own terms.

**Cause**
The comment sweep of `build`, `tools`, `tests` and `scripts` was held to comment text only, so it changed the module docstring but not these two argparse strings in `main()` (`tools/set.py`, the `ArgumentParser(description=...)` and the `--autorun` `help=`). They are program output, the same text a docstring becomes when argparse prints it, and they make the comparison the comment rule removes from comments.

**Change**
Drop the comparison from both strings: the description becomes "Configure an Ours build directory: write args.gn and run gn gen.", and the `--autorun` help ends "(written into the kernel command line as ours.autorun.boot=); the program must be in the image". No behaviour changes.

**Reference**
`fx set` and `zircon.autorun.boot` in Fuchsia; the change record `docs/changes/261004/comment-rule.md` lists both under its Reference.

**Acceptance**
`python3 tools/set.py --help` names neither `fx` nor `zircon`; `python3 tools/set.py --list` still runs.
