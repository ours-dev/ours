# build outstanding repairs

## 1. `vendor/cxxopt`, `vendor/googletest` and `vendor/nlohmann_json` are uninitialised submodules whose directories hold files, and nothing says the files are the pinned ones

**Severity**
Medium: the same hazard as `system/lib/concurrent` had, with a lower chance of loss, since these are public upstream projects whose pinned commits can be fetched again.

**Symptom**
`git submodule status` shows the three with a `-`, and each directory holds files (18, 19 and 25 entries on 2026-10-14). The superproject sees one gitlink each. `git submodule update --init` would replace the files with the pinned commits, and whatever differs, a local patch or a newer upstream copy, would go with them. Whether anything differs was not checked.

**Cause**
Same as the `concurrent` and `lockdep` case, which was fixed on 2026-10-14: the files were put in the directory without going through the submodule, so no repository records them.

**Change**
Check first, decide after. For each, compare the directory with the pinned commit (`git archive <pin>` from a clone of the upstream URL in `.gitmodules`, diffed with the directory). If identical, move the directory aside and `git submodule update --init`. If it differs, the difference is the user's to decide: a patch to push to a fork of its own, or a version to pin.

**Reference**
None, this project's own layout.

**Acceptance**
`git submodule status` shows no `-` for any of the three, and `ours-verify` passes.
