# build outstanding repairs

## 1. The source of `system/lib/concurrent` and `system/lib/lockdep` that the kernel builds from is tracked by nothing: they are uninitialised submodules, and no stored commit holds it

**Severity**
High, and silent: the kernel builds from this code, and a fresh clone would not have it. Nothing fails until someone clones, cleans or "initialises".

**Symptom**
Latent in a checkout that already has the files, immediate in one that does not. `git submodule status` shows both with a `-` (uninitialised), and `git ls-files -s system/lib` lists them as gitlinks, so this repository sees one entry each and none of the files under them. 15 build files depend on `//system/lib/concurrent` or `//system/lib/lockdep`. In a fresh clone the two directories are empty.

`git submodule update --init` is the obvious thing to type and it destroys the code: checked on 2026-10-14 against the commits the gitlinks pin (`git --git-dir=<stored module repo> archive <commit>`, diffed with the directory on disk):
- `concurrent` pins a commit with two files, `LICENSE` and `.gitignore`. On disk there are also `BUILD.gn`, `include/`, `src/` and `test/`. Initialising leaves two files and removes the rest, and it is the only copy.
- `lockdep` pins a commit with 18 files. Seven of them differ from the 18 on disk (`README.md`, `docs/interface.md` and five headers under `include/lockdep/`), and the pinned one has `tests/` where the disk has `test/`. Its stored repository has other branches, among them `feat/gn-layout` and `import/baseline-20260917`, and the disk copy differs from each of the three in several files. Initialising replaces the version the kernel builds against with an older one, and no branch has the one on disk.

**Cause**
The repository moved from Bazel to GN and the submodules did not follow: the pinned commits describe the earlier layout, and the GN-layout code was written in the working directories. `lockdep`'s was snapshotted onto branches of its own repository at two moments and has moved on since; `concurrent`'s was committed nowhere, its stored repository holds one `Initial commit`. Nothing in this repository recorded any of it.

A second cost: a host test in these directories cannot be changed through this repository, so a change to `system/lib/concurrent/BUILD.gn` is not a change to it.

**Change**
The decision is the user's; the options:
- Make the two directories ordinary directories of this repository: `git rm --cached` the gitlinks, drop the two `.gitmodules` entries, `git add` the files. Nothing on disk changes. The upstream repositories (`ours-dev/concurrent` and `ours-dev/lockdep`) are no longer referenced from here.
- Commit the on-disk code onto a branch of each stored module repo, advance the pins to it, and only then initialise. It keeps the submodules and costs a branch for each; `lockdep`'s existing branches would have to be brought up to the disk copy first, since it differs from all of them.

Ruled out: `git submodule update --init` as it stands, for the reasons under Symptom.

**Reference**
None, this project's own layout.

**Acceptance**
A fresh `git clone` of the repository builds and boots `out/bringup.x86` without anything copied in by hand, and `git ls-files system/lib/concurrent system/lib/lockdep` lists their files.
