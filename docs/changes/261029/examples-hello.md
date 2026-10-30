# examples-hello changes

## Title: examples/hello, the finished result of the developer's guide tutorial, is built by the default build and started by no image (2026-10-29)

**Files**
- `examples/hello/BUILD.gn` — the program `hello_server` (a `user_executable`) and the component manifest `hello.cm`, both as the tutorial's level 4 leaves them; `hello.cm` is visible to `//examples/*` as well as to the image
- `examples/hello/hello_server.cpp` — echo's program from its first `#include` to the end of `main`, renamed, with `MAX_CLIENTS` 1
- `examples/hello/protocol/` — the protocol library `ours.examples.hello` (`Greet` and `Stop`), echo's with `test/` and `README.md` left out, renamed, with its own two method numbers
- `examples/BUILD.gn` — new `group("hello")` naming the program and the manifest in the user toolchain; `group("examples")` names its programs in the user toolchain too, and lists `:hello`
- `BUILD.gn` — the default group names `//examples:hello` and the protocol library's host build
- `docs/guide/02-first-server.md`, `docs/guide/README.md` — the guide says the finished result is in the tree (not part of this commit)

**Symptom**
The developer's guide chapter 2 teaches a reader to turn a copy of `examples/echo` into a server of their own, level by level, and quotes line numbers, renames and log lines. The second cold read found that a reader who makes a mistake has no known-good file to compare with, and that the chapter can silently rot: nothing builds what it describes.

**Cause**
The guide had no examples of its own (its plan chose to quote the real files). That keeps the guide short and cannot catch a drift: a rename that the table misses, a line that no longer matches, or an API that changed under the tutorial's code shows up only when a reader tries the tutorial. A copy that the default build compiles shows it on the first build after the change.

Why the copy is not started by the image. Both images `rel` and `dbg` boot a fixed set of components, and `routing_checks` reaches `echo` through the root's manifest. Adding `hello` to the root manifest would change what every boot starts and checks, to keep an example true, and it would make a reader's own `hello` collide with it. A component exists in an image only when the root manifest lists it, so building `hello.cm` without listing it is enough to compile the manifest (the build checks that `expose` names a declared capability) and not to start anything.

Why `hello.cm` needs a wider `visibility`. A `component_manifest` target is visible only to the targets that may depend on it, and echo's allows `//kernel/products/image/*` alone. The default group cannot name it, and the dev image must not change. So `hello.cm` also allows `//examples/*`, where the group `hello` lives (the example `profile` has the same two entries). The program `hello_server` keeps echo's `//examples/*`.

Why `examples/BUILD.gn` changed beyond the new group. GN evaluates a whole file when any target in it is named, and the file's one group named `echo:echo_server` and `profile` without a toolchain, which loads their files in the default toolchain, where each asserts a user toolchain. Nothing named the group, so nothing ever evaluated it. Naming `//examples:hello` evaluated it, and the build stopped at `examples/profile must be built in a //build/toolchain/ours:user-* toolchain`. Both labels now carry the user toolchain.

**Change**
The default group (`BUILD.gn`), after the echo protocol's tests:

```gn
    "//examples:hello",
    "//examples/hello/protocol($host_toolchain)",
```

The group in `examples/BUILD.gn`:

```gn
group("hello") {
  deps = [
    "hello:hello.cm(//build/toolchain/ours:user-$current_cpu)",
    "hello:hello_server(//build/toolchain/ours:user-$current_cpu)",
  ]
}
```

and the labels of the old group, before and after:

```gn
# before
    "echo:echo_server",
    "profile",
# after
    "echo:echo_server(//build/toolchain/ours:user-$current_cpu)",
    "profile(//build/toolchain/ours:user-$current_cpu)",
    ":hello",
```

The files of `examples/hello` are what the tutorial produces: the library is echo's, `test/` and `README.md` deleted, the blank line and `tests` group at the end of its `BUILD.gn` deleted, every name replaced by the rename table of the chapter's level 0, and the two method numbers set to what `system/lib/wire/test/ordinals_test.py` prints; the program is echo's lines 27 to 300 with the same table and `usize{1}`; the `BUILD.gn` is echo's without its comments. Every file is then formatted with `scripts/format_cpp.py` and `scripts/format_gn.py`: renaming `echo_string` to `greet` shortens lines, and clang-format joins some of them, so the formatted file is the answer. A script derived these files from `examples/echo` by those rules alone, and the same rules with the hand-run edits of the guide's second revision give the same bytes before formatting.

Rejected: a `hello` child in the dev image's root manifest (changes every boot and collides with a reader's copy); the labels directly in the root group with `visibility` extended to `//:default` (puts the root group into the lines the tutorial teaches); a second example directory for the answer (two protocols would then share method numbers, which `ordinals_test.py` rejects).

**Reference**
None, this project's own design: the guide's plan (decision 2 of `.agents/tasks/server-guide/PLAN.md`) first quoted echo and wrote no example, and the maintainer chose to keep the tutorial's finished result in the tree after the second cold read.

**Verification**
`python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/guide-hello-tree`: `dbg PASS` with 44 passed, 4 skipped; `rel PASS` with 41 passed, 8 skipped, 6/6 required; `host-tests PASS` with 148 passed. The host tests are the testonly executables of the host toolchain and `hello` adds none (152 before and after, by `gn ls --testonly=true --type=executable`), so the count did not change. `gn desc //:default deps --all` reaches `//examples/hello:hello_server`, `//examples/hello:hello.cm` and the protocol library in the host and user toolchains. `python3 system/lib/wire/test/ordinals_test.py` ends with `OK`. `clang-tidy -p out/guide-hello-tree` on `examples/hello/hello_server.cpp` and `examples/hello/protocol/src/ours_examples_hello.cpp` reports nothing. `scripts/format_cpp.py --check` and `scripts/format_gn.py --check` are clean on every file added or changed. `git diff` touches nothing under `kernel/products/image`.

The guide's instructions were then followed literally in a second worktree made from this commit (a script that finds each edit by the text the chapter names and takes the reader-written code out of the chapter's own code blocks): the finished copy set aside with `rm -r examples/hello` and the block of the root `BUILD.gn` deleted, levels 0 to 5 applied and booted, the formatters run. Every boot passed (`rel` 6/6 required at levels 1 to 4, then `dbg PASS` 44 passed and `rel PASS` 41 passed with 8/8 required and 148 host tests at the end). At the end `git status examples/hello` and `git diff -- examples/hello` printed nothing: what the tutorial produces is byte for byte this directory. That run found three things in the chapter, now fixed there: the `BUILD.gn` of the protocol ends with a blank line unless the line above its `tests` group goes too; renaming shortens lines, so `scripts/format_cpp.py` changes seven places in the program and the library, and the answer is the formatted text; and one reader-written block was not clang-format clean.

**Deliberately not done**
`hello` is not in the dev image, so no boot starts it and nothing routes to it: the answer is compiled, not run. Its protocol has no host test, as the tutorial's level 0 leaves it. The tutorial's edits to `routing_checks` and `boot_check.py` are not made here, because they are the tutorial's steps for the reader's own tree; the guide says how to remove this copy before doing them.

**How it was found**
None for a bug in `hello`. The stale group in `examples/BUILD.gn` showed when the first boot after wiring stopped at the `profile` assert, which names the include chain (`See //examples/BUILD.gn:23:5: which caused the file to be included`).

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why must the default group name `hello_server` and `hello.cm` with the user toolchain, and what stops the build when a label leaves it out?
2. Why does `hello.cm` allow `//examples/*` in its `visibility`, and why is the dev image's root manifest not the place to build it?
3. If the tutorial's rename table missed a name, how would the finished copy in the tree show it, and which command compares a reader's copy with it?

## Title: examples/hello follows echo, a client is told when its wait cannot be armed again (2026-10-30)

**Files**
- `examples/hello/hello_server.cpp` — `Client` overrides `on_rearm_failed()` and hands the call to the new `HelloServer::rearm_failed()`, which frees the entry and logs the cause; the comment of `Client` says the order is the same as for `Close`. The three changes are echo's, renamed.
- `docs/guide/01-concepts.md`, `02-first-server.md`, `04-tree-servers.md`, `05-testing-and-debugging.md`, `06-checklist.md` — the guide says the same (not part of the example's commit)

**Symptom**
Latent, cannot be triggered today. `docs/changes/261029/loop.md` gave `loop::Waiter` a second virtual function, `on_rearm_failed(Status)`, and `examples/echo/echo_server.cpp` was changed to use it. `examples/hello` is what the guide's tutorial produces from echo, so it has to follow: otherwise a reader who follows the tutorial gets a `hello_server.cpp` that differs from the finished copy, and the copy keeps the leak that echo had.

**Cause**
The guide's finished copy is a rename of echo's program from its first `#include` to the end of `main`. Any change to those lines of echo is a change to the copy, and nothing compares the two: the default build compiles the copy, which stays true to the Loop's interface, and not to echo's text.

**Change**
The 19 lines that echo gained, renamed by the guide's table (no echo name occurs in them):

```cpp
        auto on_rearm_failed(Status const cause) -> void override {
            server->rearm_failed(*this, cause);
        }
```

```cpp
    auto rearm_failed(Client &client, Status const cause) -> void {
        client.channel = Uandle::Invalid;
        report("cannot wait on a client again", cause);
    }
```

The file was derived again from echo by the rules of the first section of this record (the table, `usize{1}`, the formatters) and compared with the committed one: only these hunks differ.

**Reference**
None, this project's own design: the same pattern as `LoopServer::Binding::on_rearm_failed()`, `docs/changes/261029/loop.md`.

**Verification**
`boot_check.py --out out/guide-sync`: `dbg PASS` (44 passed, 4 skipped), `rel PASS` (41 passed, 8 skipped, 6/6 required), `host-tests PASS` (151 passed); `ordinals_test.py` ends `OK`; `clang-tidy` reports nothing on `hello_server.cpp`; `format_cpp.py --check` is clean. A second worktree from that commit followed the guide literally (the finished copy set aside, levels 0 to 5 applied, the formatters run): levels 3 and 4 booted with `routing_checks: hello checked`, the full command passed with `dbg` 44, `rel` 41 with 8/8 required and 151 host tests, and `git status examples/hello` and `git diff -- examples/hello` printed nothing.

**Deliberately not done**
A test that fails when echo and hello drift apart. The guide's literal run is that test, by hand; making it a script would need a rule for what in echo is the tutorial's and what is echo's own.

**How it was found**
By the list of changed lines in `.agents/tasks/loop-waiter-notify/RESULT.md`, which names the guide and `examples/hello` as the next step.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why does the Loop call `on_rearm_failed()` after it has freed the slot, and not before, and what may the override therefore not do?
2. Why is it enough that `examples/hello` is a renamed copy of echo's lines for it to need this change?
3. Which command shows that a reader's copy of the tutorial equals the committed one?
