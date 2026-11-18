# ci outstanding repairs

## 1. Host tools pair LLVM 23's libc++ headers with the system's libc++, so they link only on macOS 26 or later

**Severity**
Medium: the build fails on any macOS before 26; it works on the user's macOS 27 and on the macos-26 CI runner, so nothing breaks today.

**Symptom**
On the GitHub macos-15 runner (run 44227709852, 2026-11-18) the host link of `obit` failed:

```text
Undefined symbols for architecture arm64:
  "std::__1::__hash_memory(void const*, unsigned long)", referenced from:
      cxxopts::OptionDetails::OptionDetails(...)
ld: symbol(s) not found for architecture arm64
```

**Cause**
Host compiles use the prebuilt clang's own libc++ headers (`-nostdinc++` plus `prebuilt/clang/llvm-23/include/c++/v1`, `//build/config:kernel_libcxx_include`) but link the platform's `/usr/lib/libc++.1.dylib` through the SDK (`//build/config:mac_host_sysroot`). Headers and library come from different libc++ releases. LLVM 21 moved `std::__hash_memory` out of line into the dylib; the headers choose between the exported declaration and the old inline body with `_LIBCPP_AVAILABILITY_HAS_HASH_MEMORY`, which only means anything when vendor availability annotations are on, and the upstream release's `__config_site` has `_LIBCPP_HAS_VENDOR_AVAILABILITY_ANNOTATIONS 0`. So every `std::unordered_map<std::string, ...>` in a host tool calls into the dylib, and only a system libc++ that already exports the symbol links: the macOS 26 and 27 SDKs list it in `usr/lib/libc++.tbd`, the macOS 15.5 SDK does not. The next out-of-line move in libc++ will raise the floor again, silently.

**Change**
Link host tools against the libc++ of the same release as the headers: keep `lib/libc++.a` and `lib/libc++abi.a` of the macOS LLVM release in `build/prebuilt.json` (check that the archive ships them), and link host executables with `-nostdlib++` plus those archives, so a host tool depends on nothing of the system's C++ runtime. Ruled out: the SDK's libc++ headers for host compiles (their LLVM is older and lacks C++23 library features the host code may use); turning vendor availability annotations on by editing the release's `__config_site` (a change to vendored headers that libc++ does not support for non-Apple builds); a newer runner alone, which is what CI does today and only hides the dependency.

**Reference**
None, this project's own design. Toolchains that ship their own libc++ headers make the same choice; Chromium links its own libc++ statically into every host and target binary (`//buildtools/third_party/libc++`, `use_custom_libcxx = true`) for this reason.

**Acceptance**
`.github/workflows/ci.yaml` back on `runs-on: macos-15` builds and passes `python3 scripts/ci.py all`, and `otool -L out/ci.x86/host/obj/kernel/lib/obi/cli/obit` lists no `libc++.1.dylib`.

## 2. .gitmodules still declares system/lib/bits, which the tree no longer has

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
