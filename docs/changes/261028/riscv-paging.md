# riscv-paging changes

## Title: riscv's arch headers are listed at the paths they are at, so `gn check` reads them (2026-10-28)

**Files**
- `kernel/arch/riscv/BUILD.gn` — `//kernel/arch/riscv:headers` lists its headers under `headers` instead of `public`

**Symptom**
`gn check out/bringup.riscv "//kernel/arch/riscv/*"` stopped with `Source file not found` for `//kernel/arch/riscv/arch/riscv/boot_marker.hpp`, and `gn desc` showed all seven public headers under `//kernel/arch/riscv/arch/riscv/`, a directory that does not exist. Nothing failed to compile: the include directory itself was right, so every `#include <arch/riscv/...>` resolved.

**Cause**
`cpp_interface()` (`build/cpp/cpp_interface.gni`) takes the header list in one of two spellings. `public` is forwarded to the `source_set` as it is, so GN reads each entry relative to the BUILD.gn file; `headers` is the spelling that gets `include_dir` put in front of each entry. riscv's list was written with `public` but in the include-relative form (`arch/riscv/sbi.hpp`), so GN looked for every header one `lib/arch/include` too high. The headers were therefore owned by no target: `gn check` cannot tell a dependent that includes one without depending on `:headers` from one that does, and an IDE indexing the target finds none of them. x86 made the same mistake once and fixed it the same way (its comment in `kernel/arch/x86/BUILD.gn`).

**Change**
Before:

```text
cpp_interface("headers") {
  include_dir = "lib/arch/include"
  public = [
    "arch/riscv/boot_marker.hpp",
```

After:

```text
cpp_interface("headers") {
  include_dir = "lib/arch/include"
  headers = [
    "arch/riscv/boot_marker.hpp",
```

with a comment saying why. Landed before the Sv39 headers join the list, so that they are listed where they are from the start.

**Reference**
None, this project's own build template; x86's `//kernel/arch/x86:headers` is the precedent.

**Verification**
- `gn check out/bringup.riscv "//kernel/arch/riscv/*"`: `Header dependency check OK`; `gn desc ... public` lists `//kernel/arch/riscv/lib/arch/include/arch/riscv/boot_marker.hpp` and the rest.
- riscv `bootshim.bin` still links.
- `ours-verify --out out/x86-paging`: build ok, dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), host-tests PASS (150 passed); x86 does not read this file.

**Deliberately not done**
None.

**How it was found**
Adding the Sv39 headers to this list meant reading `cpp_interface()` to see which spelling the entries needed; `gn check` and `gn desc` then confirmed the existing entries were already wrong.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why did every `#include <arch/riscv/...>` still compile while GN could not find a single one of those headers?
- What does `gn check` lose when a header is listed under no target?
- Why does a header-only library have a `source_set` at all?
