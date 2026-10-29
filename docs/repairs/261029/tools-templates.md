# tools-templates outstanding repairs

## 1. The `cpp_library` template names the target after the library, so the test's dependency on the directory label does not resolve for a protocol library

**Severity**
Low: `gn gen` fails with a clear message and the fix is one rename, but it fails on the first attempt to scaffold any of the tree's protocol libraries the way the template's own help says.

**Symptom**
Scaffolding a library whose name differs from its directory's last component, for example `name=ours_examples_hello` into `examples/hello/protocol` with `gn_path=examples/hello/protocol`, writes `source_set("ours_examples_hello")` in `BUILD.gn` and `deps = [ "//examples/hello/protocol" ]` in `test/BUILD.gn`. GN reads the second as `//examples/hello/protocol:protocol`, so `gn gen` stops with "Unresolved dependencies ... needs //examples/hello/protocol:protocol" (reproduced on 2026-10-29 with a minimal GN project of that shape, in a scratch directory). The tree's protocol libraries all have this shape: `examples/echo/protocol` names its target `protocol`, and `system/lib/ours.resource` names its target `ours.resource` while its C++ name is `ours_resource` (`system/lib/ours.resource/BUILD.gn:10-14`).

**Cause**
`tools/templates/cpp_library/BUILD.gn.jinja:4` names the target `{{ name }}`, and `tools/templates/cpp_library/test/BUILD.gn.jinja:8` depends on `//{{ gn_path }}`, which is only the same target when `name` is the last component of `gn_path`. That holds for `kernel/lib/gktl` and its kind, and for no protocol library.

**Change**
Name the target after the last component of `gn_path`, and keep `name` for the include prefix and the namespace, or write the test's dependency as `//{{ gn_path }}:{{ name }}`. The second is the smaller change. The template also leaves out what a protocol library needs: `public_deps` on `//system/lib/wire` and `//system/lib/wire:headers`, `visibility = [ "*" ]` and a `tests` group. Whether to add a second template for protocol libraries is for the user to decide.

**Reference**
None, this project's own tooling.

**Acceptance**
Scaffolding `ours_examples_hello` into `examples/hello/protocol` and naming `//examples/hello/protocol:tests` in `//:default` passes `gn gen`, and the placeholder test builds and runs.

## 2. A library made from the copier template says its tests run on the host, and nothing runs them until a hand-written line is added

**Severity**
Low: the test is real and passes when run by hand, and the missing step is written down, but only in Chinese and as optional. The cost is a test that looks wired and is not, which is the state `BUILD.gn` records for tests that "sat unwired for a while".

**Symptom**
Right after `python3 -m copier copy tools/templates/cpp_library <dir>`, the library has `test/BUILD.gn` with an `executable` that is `testonly`, and its generated README says tests "run on the host, not in the kernel". `scripts/host_tests.py` does not find that executable, so neither the `host-tests` line of `boot_check.py` nor a plain build runs it, and the boot check stays green whether the test passes or fails. It is found only once a `tests` group names it and the root `BUILD.gn` names that group in `//:default`.

**Cause**
`tools/templates/cpp_library/BUILD.gn.jinja` writes the library and its `headers` target and no `group("tests")`, and nothing in the template touches the root `BUILD.gn`. `scripts/host_tests.py` takes the executables that `//:default` reaches (`gn desc //:default deps --all`), and the root list is written by hand. The step is stated in `tools/templates/README.md:37`, in Chinese, as "if it should take part in the test roll-up, hang the target in `test/` on the matching test group". The generated README (`README.md.jinja`, "Building and testing") says nothing of it, and the generated test target is the only place a reader looks next.

**Change**
Make the template write the group, and say the last step where the reader sees it. In `BUILD.gn.jinja`, add `group("tests") { testonly = true; public_deps = [ "test:{{ name }}($host_toolchain)" ] }`, as `system/lib/loop/BUILD.gn:28-31` has it. In the generated README, one sentence: the group has to be named in the `default` group of the root `BUILD.gn`, or nothing builds or runs the test. Ruled out: having the template edit the root `BUILD.gn`; the template writes into the new library's directory (`tools/templates/cpp_library/copier.yml:3`), and a library may live anywhere.

**Reference**
None, this project's own tooling.

**Acceptance**
A library scaffolded with the template has a `tests` group and a README that says to name it in `//:default`. After the line is added, `prebuilt/gn/gn ls out/bringup.x86 --testonly=true --type=executable` lists the new test, and the `host-tests` count of `boot_check.py` rises by one.
