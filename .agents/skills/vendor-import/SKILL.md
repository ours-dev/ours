---
name: vendor-import
description: Import a third-party C/C++ library into this repo's vendor/ tree and wire it up to GN via build/secondary/, following the existing vendor/cxxopt pattern. Use whenever the user asks to add, vendor, or import a third-party library (e.g. "add nlohmann/json", "vendor in googletest", "pull in <library>") — never hand-roll something a well-known library already does without asking first (see feedback_third_party_libs_need_approval memory); this skill is how an approved library actually gets added to the tree.
---

# Vendor Import

Pulls a third-party library's source into `vendor/<name>` and exposes it to the
GN build via a `BUILD.gn` placed under `build/secondary/vendor/<name>/`, using
GN's `secondary_source` mechanism (see `.gn`: `secondary_source =
"//build/secondary/"` — GN looks for a target's `BUILD.gn` there when one
isn't present directly in the source tree). `vendor/cxxopt` +
`build/secondary/vendor/cxxopt/BUILD.gn` is the reference example this skill
follows exactly; read it before starting if anything below is ambiguous.

## When NOT to use

- The functionality is small enough to hand-roll and the user hasn't approved
  a third-party dependency for it — ask first (this is a standing preference,
  not a one-off).
- The library is needed by kernel/freestanding code. Everything vendored this
  way is host-side C++ (build tooling, host tests) — this repo's kernel code
  doesn't get third-party deps through this path. If in doubt, ask.
- The library already exists under `vendor/` — check first (`ls vendor/`).

## Step 1 — Confirm before cloning

Nail down with the user, if not already stated:
- Exact library name and the GN target name it should expose (usually the
  same, snake_case if the upstream name has hyphens/slashes).
- Upstream git URL.
- Pinned version: a **tag**, not a moving branch. Ask which release, or use
  the latest stable tag if the user just says "the latest".

Do not clone until these are settled — this is a repo-visible change (new
files, new BUILD.gn, new build dependency) and warrants the same confirmation
as any other dependency addition.

## Step 2 — Shallow-clone the pinned tag into vendor/

Mirrors how `vendor/cxxopt` was added: a shallow clone of the tag, kept as a
real (if disconnected) git checkout — not stripped of its `.git`, not turned
into a git submodule (unlike `system/lib/*`, `vendor/*` entries are **not**
listed in `.gitmodules` — check `vendor/cxxopt`'s presence there is absent to
confirm this convention before deviating from it).

```bash
scripts/vendor-clone.sh <name> <git-url> <tag>
```

(Run from the repo root; see `scripts/vendor-clone.sh` in this skill directory
— it just wraps `git clone --depth 1 --branch <tag> <url> vendor/<name>` with
a check that `vendor/<name>` doesn't already exist.)

## Step 3 — Decide the target shape: header-only vs compiled

Look at what you cloned:

**Header-only** (e.g. a single amalgamated header, or a header-only
`include/` tree with no `src/*.cc`/`*.cpp` to compile): use a `source_set()`
that's just a `public_configs` include-path forward, exactly like
`vendor/cxxopt`:

```gn
# build/secondary/vendor/<name>/BUILD.gn
# <Library> — <one-line description>.
# Release: <tag>
# Upstream: <url>

config("<name>.config") {
  include_dirs = [ "//vendor/<name>/<path-to-include-root>" ]
}

source_set("<name>") {
  public_configs = [ ":<name>.config" ]
  sources = [ "//vendor/<name>/<path-to-include-root>/<main-header>" ]
  visibility = [ "*" ]
}
```

**Compiled** (real `.cc`/`.cpp` translation units to build, e.g. googletest's
`googletest/src/gtest-all.cc`): the library usually needs two include paths —
a **public** one (`include/`) for consumers, and a **private** one (the
library's own root, for its internal headers) used only while compiling the
library's own sources. Keep the private config un-exported:

```gn
config("<name>.config") {
  include_dirs = [ "//vendor/<name>/<public-include-root>" ]
}

config("<name>.internal_config") {
  include_dirs = [ "//vendor/<name>/<private-root>" ]
  visibility = [ ":*" ]  # only this BUILD.gn's own targets use it
}

source_set("<name>") {
  public_configs = [ ":<name>.config" ]
  configs += [ ":<name>.internal_config" ]
  sources = [ "//vendor/<name>/<path-to-all-in-one-or-listed-sources>.cc" ]
  visibility = [ "*" ]
}
```

Prefer an upstream-provided "all-in-one" translation unit (like
`gtest-all.cc`) over listing every source file individually when the library
ships one — it's what the library's own build systems (CMake, Bazel) do, and
it keeps this BUILD.gn from drifting out of sync with upstream's file list.

## Step 4 — Write the BUILD.gn, then verify

1. Create `build/secondary/vendor/<name>/BUILD.gn` following Step 3's
   template — do **not** put it at `vendor/<name>/BUILD.gn` directly, that
   defeats the point of `secondary_source` (keeps generated/non-upstream
   build files out of the vendored tree, so a future re-vendor of a newer
   tag can just delete-and-reclone `vendor/<name>` without touching the
   build file).
2. Confirm the target resolves and compiles: add a throwaway consumer (or
   reuse an existing one if this import was requested to unblock a specific
   caller) and run `gn gen` + `ninja` on it via `docker exec dev-common` (see
   this project's own container-build conventions — `gn`/`ninja` only exist
   inside the `dev-common` container, not on the host).
3. Report back: target label (`//vendor/<name>:<name>`), what it exposes
   (public headers reachable via `#include <...>`), and the pinned version.

## Common pitfalls

- Forgetting `visibility = [ "*" ]` on the public target — every other vendor
  target in this repo sets it, matching the repo's general "libraries are
  globally visible, only application-shaped targets restrict visibility"
  convention.
- Using a floating branch (`main`, `master`) instead of a tag — breaks
  reproducibility; always pin.
- Putting the BUILD.gn inside `vendor/<name>/` instead of
  `build/secondary/vendor/<name>/` — GN will simply fail to find the target
  metadata the way this repo expects, or worse, will pick up an
  upstream-shipped BUILD.gn/BUILD.bazel/CMakeLists.txt that wasn't written
  for this build.
