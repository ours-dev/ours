# ci outstanding repairs

## 1. .gitmodules still declares system/lib/bits, which the tree no longer has

**Severity**
Low: git ignores a declaration without a gitlink, so no checkout, clone or CI run fails today.

**Symptom**
`git config -f .gitmodules --get-regexp 'submodule\.system/lib/bits\..*'` prints a path and an SSH URL, while `git ls-files -s system/lib/bits` prints nothing and the directory does not exist. A clean clone with `--recurse-submodules` (the CI dry run of 2026-11-17) fetches six submodules and silently skips this one.

**Cause**
The library was removed from the tree, or never committed as a gitlink, without its `.gitmodules` entry. A reader of `.gitmodules` takes it for a dependency, and the CI token (`SUBMODULE_TOKEN`) would be scoped to a private repository the build never reads.

**Change**
Delete the `system/lib/bits` section from `.gitmodules` (`git config -f .gitmodules --remove-section submodule.system/lib/bits`), after checking with the user that `bits` is not about to come back as a gitlink.

**Reference**
None, this project's own design.

**Acceptance**
`git config -f .gitmodules --get-regexp 'submodule\..*\.path'` lists exactly the paths `git ls-files -s | grep ^160000` lists.
