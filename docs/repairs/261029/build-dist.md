# build-dist outstanding repairs

## 1. A build directory one level deeper than out/<name> fails the default build at //tests/dist-smoke:with_rename_dist

**Severity**
Medium: the whole default build fails, but only in a build directory whose path below the repo root has three or more components, such as `out/concurrency/bringup.x86`; every directory `tools/set.py` makes by default is `out/<name>`, two components.

**Symptom**
On 2026-10-29, `tools/set.py bringup.x86 --out out/concurrency/cov-kernel --variant coverage-kernel` followed by `ninja -k 0 -C out/concurrency/cov-kernel` failed exactly one edge of 1055, and a review run saw the same for `tools/set.py bringup.x86 --out out/verify-e7ee/bringup.x86`:

```text
FAILED: [code=1] gen/tests/dist-smoke/with_rename.fini.dist
Exception: ERROR: Renamed distribution entries have unknown source destination:
  - {"destination": "bin/renamed", "keep_original": false, "label": "//tests/dist-smoke:renamed(//build/toolchain/ours:kernel-x86)", "renamed_source": "../../../../tests/dist-smoke/hello.txt"}
```

Everything else builds, the images and the host tests included, so `ninja -k 0` and then a boot script's `--no-build` work around it; a plain `ninja -C`, `boot_check.py` or `cov_kernel.py` stops at it. In that directory's `gen/tests/dist-smoke/with_rename.fini.dist.partial` the plain entry's `source` is `../../../tests/dist-smoke/hello.txt` and the renaming entry's `renamed_source` is `../../../../tests/dist-smoke/hello.txt`, one `../` more. In `out/bringup.x86` both are `../../tests/dist-smoke/hello.txt`.

**Cause**
The path is rebased twice. `tests/dist-smoke/BUILD.gn` passes `renamed_binary("renamed")` the argument `source = rebase_path("hello.txt", root_build_dir)`: a path already relative to the build directory, `../../../tests/dist-smoke/hello.txt` in a three-component one. `renamed_binary()` (`build/dist/renamed_binary.gni`) expects a GN path, as its own example `source = "$root_out_dir/foo"` shows, and rebases it itself: `_rebased_source = rebase_path(invoker.source, root_build_dir)`. GN reads a relative input to `rebase_path()` as relative to the current directory, `//tests/dist-smoke/`, so the second rebase walks three `../` up from there, one past the source root, and the result needs four `../` from the build directory.

In a two-component build directory the mistake cancels out: `//tests/dist-smoke/` and `//out/<name>/` are both two levels below the root, so going up two from either lands on the root, and `//tests/dist-smoke/../../tests/dist-smoke/hello.txt` is the right file. That is why every `out/<name>` builds and nobody saw it. `expand_and_find_conflicting_dist_entries.py` then matches a renaming entry to a plain one by its source string, finds no plain entry with the four-`../` source, and refuses the manifest.

`renamed_binary()` has no other caller in the tree, so only the smoke test is affected. The plain entry in the `renamed_source` group is right as it is: `distribution_entries` hold paths relative to the build directory, which is what its `rebase_path("hello.txt", root_build_dir)` gives.

**Change**
In `tests/dist-smoke/BUILD.gn`, pass the GN path and let the template rebase it once: `source = "hello.txt"` in `renamed_binary("renamed")`. Leave the template alone: its callers are meant to pass GN paths such as `"$root_out_dir/foo"`, and making it accept rebased paths too would break those. Optionally say in the template's comment for `source` that it is a GN path, not one relative to the build directory.

**Reference**
None, this project's own build. `build/dist/renamed_binary.gni` is adapted from Fuchsia's `//build/dist/renamed_binary.gni`, which rebases `invoker.source` the same way; its callers pass GN paths.

**Acceptance**
- `tools/set.py bringup.x86 --out out/a/b` then `ninja -C out/a/b` (no `-k`) succeeds, and `out/a/b/gen/tests/dist-smoke/with_rename.fini.dist` lists `bin/renamed` and no `bin/original`.
- In `out/bringup.x86`, `gen/tests/dist-smoke/with_rename.fini.dist` is unchanged.
- `ours-verify` passes on x86.
