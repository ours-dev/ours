# wire outstanding repairs

## 1. The first design of the framing says an unknown or malformed message gets an epitaph, and the code and the later design say it gets none

**Severity**
Low: the code is consistent with itself and with `docs/design/261004/component-framework.md`, and nothing misbehaves. A reader who follows the comment in `wire.hpp` to its source is told the opposite of what every server does.

**Symptom**
`system/lib/wire/include/wire/wire.hpp:19-20` names `docs/design/260924/userspace-filesystem.md` §2.1 as the source of the framing. Section 2.5 of that document (lines 163-164) says a malformed message is answered with an epitaph `InvArgs` and an unknown ordinal with an epitaph `NotSupported`, and line 858 repeats it ("先发 epitaph（InvArgs 或 NotSupported）再关"). Every server in the tree does the opposite: it closes the connection and writes nothing (`examples/echo/echo_server.cpp:172-191`, `system/lib/ioservice/include/ioservice/server.hpp:83-90`, `system/lib/ioservice/src/server.cpp:562-565`), and `docs/design/261004/component-framework.md:323` records the rule ("close the connection without an epitaph"). The 260924 document carries "Superseded" banners on other sections (lines 336 and 374) but none on 2.5 or on the table at line 858.

**Cause**
The rule changed on 2026-10-04 with the component-framework design, which edited the sections it replaced and left section 2.5 as written. The header comment still points at the document as a whole.

**Change**
Add a one-line banner under the 2.5 heading and beside the table at line 858, in English, saying that the rule is superseded by `docs/design/261004/component-framework.md` ("The protocol subset": a malformed message or an unknown ordinal closes the connection without an epitaph), as the document already does for its other superseded sections. Ruled out: rewriting the Chinese text, which the repository's rule leaves as it is.

**Reference**
None, this project's own design.

**Acceptance**
`grep -n "epitaph" docs/design/260924/userspace-filesystem.md` shows a banner at section 2.5 and at the table near line 858 pointing at the 261004 design.

## 2. `ordinals_test.py` is run by nothing, so a wrong or copied method number passes the boot check

**Severity**
Low: no ordinal in the tree is wrong today (the test passes), and a server and its clients in one build share one header, so a wrong number is wrong on both sides alike. It would show only when a peer built from the selector alone meets the library, or when a copy-pasted number makes two methods of one protocol the same.

**Symptom**
`python3 system/lib/wire/test/ordinals_test.py` is "run by hand" (`system/lib/wire/BUILD.gn:26-27`). No script, skill, hook or GN target runs it: it is not in `//:default`, `.agents/skills/ours-verify/scripts/boot_check.py` does not call it, and neither does `.githooks/pre-commit`. A new protocol library whose `.ordinal` is typed wrong, or copied from another method, builds, passes its own host tests and boots.

**Cause**
The test is Python and the host-test finder (`scripts/host_tests.py`) runs only the testonly GN executables of the host toolchain, so a Python test has no way in. `system/test_component_manifest.py` is a Python test of the same kind; whether it is run by anything was not checked here.

**Change**
Run it from `boot_check.py` beside the host tests, with its output in `host-tests.log` and a failure that fails the check, or from the pre-commit hook when a `wire::Method` line changed. The first is the smaller change and reaches every worktree. Ruled out: a GN action, which would put Python in the build graph for one check.

**Reference**
None, this project's own tooling.

**Acceptance**
Changing one digit of `ECHO_STRING`'s ordinal makes `boot_check.py` print a failure that names the selector and the right value, and putting it back makes it pass.
