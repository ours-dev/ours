# server-guide-first-server: chapter 2, the first-server tutorial, written and run

**Date**: 2026-10-29

## Delivered

- `docs/guide/02-first-server.md`, 551 lines, uncommitted. A tutorial in levels 0 to 5 that grows `examples/hello` from a copy of `examples/echo`. Every level was built and booted in a scratch worktree (`.worktrees/guide-hello`, based on `2893967c`, removed at the end), and the chapter quotes only log lines and error messages seen in those runs. 20 snippets are copied verbatim from the committed tree (all at most 15 lines), 27 `file:line` citations and 28 links with anchors resolve (`/private/tmp/claude-501/-Users-smallhuazi-Workspaces-projects-ours-dev-ours-refactor-reasonix/a245d98c-7a59-476d-ab41-70d54031516d/scratchpad/verify_chapter.py`, run against `git show HEAD:<path>`).
- Two repair items for defects of the tree found on the way: `docs/repairs/261029/routing-checks.md` item 2 and `docs/repairs/261029/echo.md` item 3.
- Nothing is committed or staged. The hash in the chapter's last line is `3cb0d8a5`, the answer of `git log -1 --format=%h -- system examples tools scripts build kernel/products` (the revision rulings of `.agents/tasks/server-guide/PLAN.md`, 3a); plain `git log -1` says `109f8ba3`, a commit that touched only `kernel/mm` and a skill.

## Verification

Fourteen boots in all, each `python3 .agents/skills/ours-verify/scripts/boot_check.py --only rel --out out/guide-hello` (the first one cold, 45 s; the rest 7 s). Four more failures were provoked with `ninja` alone, without a boot. Logs were read with `/usr/bin/grep -a`. The `dbg` image was not booted, as instructed. Host tests ran in the boots without `--no-tests` and always passed (148).

### Baseline, the untouched tree (boot 1)

The run used `--out out/guide-hello`; the block below has the two path fragments of the chapter's "Before you start" (`out/hello`, `ours-verify-hello`) as the chapter prints them, which is what the naming rule gives for `--out out/hello`.

```text
configure  /Users/smallhuazi/Workspaces/projects/ours-dev/ours.refactor.reasonix/.worktrees/guide-hello/out/guide-hello is not configured; running tools/set.py bringup.x86 --out /Users/smallhuazi/Workspaces/projects/ours-dev/ours.refactor.reasonix/.worktrees/guide-hello/out/guide-hello
build      ok (/Users/smallhuazi/Workspaces/projects/ours-dev/ours.refactor.reasonix/.worktrees/guide-hello/out/guide-hello, 34.0s)
rel        PASS  shell finished at 4.6s · 41 passed, 8 skipped, 0 failed · 6/6 required · 0 error line(s) · reached: end
host-tests PASS  148 passed, 0 failed · 10.9s · log /var/folders/n_/20ryw2px3052ggtjf0xpsc8h0000gn/T/ours-verify-guide-hello/host-tests.log
logs       /var/folders/n_/20ryw2px3052ggtjf0xpsc8h0000gn/T/ours-verify-guide-hello/{rel}.log
python3 .agents/skills/ours-verify/scripts/boot_check.py --only rel --out   338.83s user 59.84s system 881% cpu 45.242 total
exit=0
```

Log: `init: started echo`, `echo: stopping, as a client asked`, `init: echo exited with 0`, `routing_checks: passed`, `init: routing_checks exited with 0`. The two `routes nowhere` lines for `denied` and `ours.test.Unoffered` are in it, from before any edit.

### Level 0, the protocol library

Edits: `mkdir examples/hello`; `cp -R examples/echo/protocol examples/hello/protocol`; delete `test/`, `README.md` and the `tests` group of `BUILD.gn`; rename `include/ours_examples_echo` and the two files inside; replace in every file, in this order: `ours_examples_echo`, `OURS_EXAMPLES_ECHO`, `ours.examples.echo`, `Echo.EchoString`, `EchoString`, `ECHO_STRING`, `echo_string`, `ECHO_NAME`, `//examples/echo`, `Echo` (done with a ten-line Python script). `grep -ri echo examples/hello/protocol` then printed nothing. Run `python3 system/lib/wire/test/ordinals_test.py`: four failures, two "hash" and two "unique" (echo's numbers copied). It names `0x0b34f151d5b5de80` for `ours.examples.hello/Hello.Greet` and `0x3b42cec6d122813b` for `Hello.Stop`; pasted as `0x0b34'f151'd5b5'de80` and `0x3b42'cec6'd122'813b`; second run: `Ran 5 tests ... OK`.

### Level 1, the root starts it (boot 2)

Edits: `examples/hello/hello_server.cpp` (`#include <ort/debug.hpp>`, `#include <ort/main.hpp>`, a `main` that calls `ort::debug_line("hello: started")` and returns 0); `examples/hello/BUILD.gn` (echo's with the names changed, no comments, no `deps`, no `manifest = {}`); a child entry `hello` after `echo` in `kernel/products/image/dev/BUILD.gn`. Result: build 0.8 s, `rel PASS ... 6/6 required`, `/boot has 14 files, 7 executable` (was 12 and 6). Log lines, in order:

```text
init: started hello
hello: started
init: hello exited with 0
```

Friction: leaving out `#include <ort/main.hpp>` (ninja only): `ld.lld: error: undefined symbol: main`, `>>> did you mean to declare main(int, char**) as extern "C"?`. The chapter says so at level 1.

### Level 2, a client can reach it (boots 3 to 5)

Edits: `hello_server.cpp` is the renamed `echo_server.cpp` lines 27-50, `SLOTS = ioservice::Outgoing::MAX_CONNECTIONS` with the `static_assert`, lines 63-84, a `HelloServer` that derives from `ioservice::Handler` and whose `connect` calls `close_with_epitaph(channel, Status::NotSupported)`, lines 255-264 without `ECHO_STORAGE`, and lines 266-300 with `HelloServer server;`. `BUILD.gn` gets echo's `deps` and `manifest` scope back. Offer, `use`, dependency and a `check_hello` of one line (`expect("a connection to hello", connect_status(HELLO), Status::NotSupported)`) as in the diff at the end of this section. Boot 3: `rel PASS ... 6/6 required`; log has `init: started hello`, no `init: hello exited`, no `routes nowhere` line naming hello, `routing_checks: passed`.

Provoked failures: boot 4, expected status changed to `NoResource`: `rel FAIL  2 FAILED line(s) ... 5/6 required`, with `routing_checks: a connection to hello = NotSupported (expected NoResource) FAILED`. Boot 5, the offer removed: `init: routing_checks's use of "ours.examples.hello.Hello" routes nowhere: NotFound at . (offer of "ours.examples.hello.Hello")` and `routing_checks: a connection to hello = NotFound (expected NotSupported) FAILED`. With ninja alone: `expose` spelled `ours.examples.hello.Helo`: `component_manifest: //examples/hello:hello.cm.manifest.expose[0].from: 'self' exposes 'ours.examples.hello.Helo', which `capabilities` does not declare`; the offer's `from = "#helo"`: `component_manifest: //kernel/products/image/dev:root.cm.manifest.offer[7].from: '#helo' names no child of this component`; the dependency left out of `routing_checks`: `routing_checks.cpp:69:10: fatal error: 'ours_examples_hello/ours_examples_hello.hpp' file not found`. A later boot (12) with `HELLO_NAME` changed to `ours.examples.hello.Hallo` and the manifests left alone: the client read `NotFound` three times (`routing_checks: Greet through the sibling's expose = NotFound (expected Ok) FAILED` and two more) and the root logged nothing about it. All four are in the chapter's table "If it fails".

### Level 3, it answers a method (boots 6 and 7)

Edits: `hello_server.cpp` becomes renamed `echo_server.cpp` lines 27-199, one `        }`, lines 205-229, 251-253 and 255-300, with `MAX_CLIENTS` 1 and `HELLO_STORAGE`; `routing_checks.cpp` gets `HELLO_TEXT`, `check_hello_greet` (a renamed copy of `check_echo_string`) and a `check_hello` that prints `routing_checks: hello checked` if `FAILURES` did not grow. Boot 6: `rel PASS ... 6/6 required`; lines `routing_checks: hello checked`, `routing_checks: passed`, no `init: hello exited`. Boot 7, the server changed to answer `"goodbye"`: `rel FAIL`, with `routing_checks: hello answered "goodbye"` and `routing_checks: Greet: the answer is not what was sent FAILED`.

### Level 4, it keeps its limits (boot 8)

Edits: `hello_server.cpp` is now the renamed `echo_server.cpp` from line 27 on, whole, with `MAX_CLIENTS` 1 (a `diff` against the renamed original shows that and the 26-line header comment only). `routing_checks.cpp` gets `check_hello_unknown` (sends `ours_examples_echo::encode_echo_string`, expects `PeerClosed` from `call`), `stop_hello` (a renamed `stop_echo`), and `check_hello_first_client` / `check_hello_second_client` around them; the second client is `connect_status(HELLO)` expecting `NoResource` while the first is connected. Result: `rel PASS ... 6/6 required`, lines `hello: stopping, as a client asked`, `init: hello exited with 0`, `routing_checks: hello checked`. Provoked in boot 13 (level 5 state, `MAX_CLIENTS` 4): the second client was accepted, `connect_status` waited, and the boot failed after 60 s with `no "routing_checks: passed", "init: hello exited with 0", "routing_checks: hello checked" within 60s`, no `FAILED` line. The chapter says so at level 4; the defect is in `routing-checks.md` item 2.

### Level 5, gate it (boots 9 to 11, and 14)

Edit: two lines after `"init: echo exited with 0",` in `REQUIRED` of `boot_check.py` (diff below). Boot 9, the call to `check_hello()` replaced by `static_cast<void>(&check_hello)` (a plain deletion is a build error: unused function), without the new lines: `rel PASS ... 6/6 required`, no hello line in the log. Boot 10, the same with the new lines:

```text
build      ok (/Users/smallhuazi/Workspaces/projects/ours-dev/ours.refactor.reasonix/.worktrees/guide-hello/out/guide-hello, 0.2s)
rel        FAIL  no "init: hello exited with 0", "routing_checks: hello checked" within 60s · 41 passed, 8 skipped, 0 failed · 6/8 required · 0 error line(s) · reached: end
```

Boot 11 and the final boot 14, everything restored, full command with host tests:

```text
build      ok (/Users/smallhuazi/Workspaces/projects/ours-dev/ours.refactor.reasonix/.worktrees/guide-hello/out/guide-hello, 0.4s)
rel        PASS  shell finished at 4.5s · 41 passed, 8 skipped, 0 failed · 8/8 required · 0 error line(s) · reached: end
host-tests PASS  148 passed, 0 failed · 0.7s · log /var/folders/n_/20ryw2px3052ggtjf0xpsc8h0000gn/T/ours-verify-guide-hello/host-tests.log
logs       /var/folders/n_/20ryw2px3052ggtjf0xpsc8h0000gn/T/ours-verify-guide-hello/{rel}.log
python3 .agents/skills/ours-verify/scripts/boot_check.py --only rel --out   7.73s user 1.72s system 148% cpu 6.374 total
exit=0
```

The lines of boot 14:

```text
init: started with the root job
init: holding the IRQ and I/O port resources
init: /boot has 14 files, 7 executable
init: routing_checks's use of "denied" routes nowhere: AccessDenied at . (offer of "denied")
init: started routing_checks
init: started echo
init: started hello
init: started shell
init: started pc-ps2
init: routing_checks's use of "ours.test.Unoffered" routes nowhere: NotFound at . (offer of "ours.test.Unoffered")
init: launch failed: mapping the program's file into this process: AccessDenied
init: echo exited with 0
hello: stopping, as a client asked
routing_checks: hello checked
init: hello exited with 0
routing_checks: passed
init: routing_checks exited with 0
init: launch failed: mapping the program's file into this process: AccessDenied
```

(`hello: stopping`, `routing_checks: hello checked` and `init: hello exited with 0` came in another order in boot 8; the chapter says the order can differ.)

### The edits outside `examples/hello`, as a diff

```diff
diff --git a/.agents/skills/ours-verify/scripts/boot_check.py b/.agents/skills/ours-verify/scripts/boot_check.py
index 3e45a761..d137c937 100644
--- a/.agents/skills/ours-verify/scripts/boot_check.py
+++ b/.agents/skills/ours-verify/scripts/boot_check.py
@@ -92,6 +92,8 @@ REQUIRED = {
     "rel": [
         "routing_checks: passed",
         "init: echo exited with 0",
+        "init: hello exited with 0",
+        "routing_checks: hello checked",
         "init: holding the IRQ and I/O port resources",
         "pc-ps2: ready",
         "pc-ps2: key a",
diff --git a/kernel/products/image/dev/BUILD.gn b/kernel/products/image/dev/BUILD.gn
index 211a4c6d..a725c428 100644
--- a/kernel/products/image/dev/BUILD.gn
+++ b/kernel/products/image/dev/BUILD.gn
@@ -96,6 +96,11 @@ component_manifest("root.cm") {
       manifest = "//examples/echo:echo.cm"
       startup = "eager"
     },
+    {
+      name = "hello"
+      manifest = "//examples/hello:hello.cm"
+      startup = "eager"
+    },
     {
       name = "shell"
       manifest = "//system/app/shell:shell.cm"
@@ -189,6 +194,13 @@ component_manifest("root.cm") {
         to = "#routing_checks"
         as = "ours.test.Echo"
       },
+
+      # hello's protocol, from the sibling that exposes it, under its own name.
+      {
+        protocol = "ours.examples.hello.Hello"
+        from = "#hello"
+        to = "#routing_checks"
+      },
     ]
   }
 }
diff --git a/system/service/init/test/routing_checks/BUILD.gn b/system/service/init/test/routing_checks/BUILD.gn
index cfe221a5..c2da0301 100644
--- a/system/service/init/test/routing_checks/BUILD.gn
+++ b/system/service/init/test/routing_checks/BUILD.gn
@@ -20,6 +20,7 @@ user_executable("routing_checks") {
   sources = [ "routing_checks.cpp" ]
   deps = [
     "//examples/echo/protocol",
+    "//examples/hello/protocol",
     "//system/lib/fsio",
     "//system/lib/ours.io",
     "//system/lib/ours.resource:client",
@@ -86,6 +87,9 @@ component_manifest("routing_checks.cm") {
         protocol = "ours.test.Echo"
         type = "ours.examples.echo.Echo"
       },
+      {
+        protocol = "ours.examples.hello.Hello"
+      },
     ]
   }
 }
diff --git a/system/service/init/test/routing_checks/routing_checks.cpp b/system/service/init/test/routing_checks/routing_checks.cpp
index b2af53c1..14fb9a2f 100644
--- a/system/service/init/test/routing_checks/routing_checks.cpp
+++ b/system/service/init/test/routing_checks/routing_checks.cpp
@@ -66,6 +66,7 @@
 #include <ort/main.hpp>
 #include <ort/startup.hpp>
 #include <ours_examples_echo/ours_examples_echo.hpp>
+#include <ours_examples_hello/ours_examples_hello.hpp>
 #include <ours_io/ours_io.hpp>
 #include <ours_process/ours_process.hpp>
 #include <ours_resource/client.hpp>
@@ -95,6 +96,8 @@ constexpr auto LAUNCHER = std::string_view{"/svc/ours.process.Launcher"};
 constexpr auto UNOFFERED = std::string_view{"/svc/ours.test.Unoffered"};
 /// echo's protocol, under the name root.cm's offer gives it.
 constexpr auto ECHO = std::string_view{"/svc/ours.test.Echo"};
+/// hello's protocol, under its own name.
+constexpr auto HELLO = std::string_view{"/svc/ours.examples.hello.Hello"};
 
 /// The line check_irq_resource() takes: ISA 3, COM2's, which is free, since
 /// QEMU has no COM2 with one serial port and the kernel keeps only its own
@@ -108,6 +111,9 @@ constexpr auto RTC_INDEX_PORT = u16{0x70};
 /// What check_echo() sends echo, and expects back.
 constexpr auto ECHO_TEXT = std::string_view{"routing_checks"};
 
+/// What check_hello() sends hello, and expects back.
+constexpr auto HELLO_TEXT = std::string_view{"routing_checks"};
+
 /// The argument that makes it the spawned copy (run_child()).
 constexpr auto CHILD = std::string_view{"child"};
 
@@ -825,6 +831,98 @@ auto check_echo() -> void {
     expect("a route to echo once it has exited", answered, Status::NotFound);
 }
 
+/// Greet on |connection|: hello answers with what it was sent.
+auto check_hello_greet(Uandle const connection) -> void {
+    auto const txid = next_txid();
+    std::array<u8, wire::MAX_MESSAGE_BYTES> bytes{};
+    auto const request =
+        ours_examples_hello::encode_greet(bytes, {.txid = txid, .value = HELLO_TEXT});
+    if (!expect("Greet", status_of(request), Status::Ok)) {
+        return;
+    }
+    Incoming reply;
+    if (!expect("Greet through the sibling's expose", call(connection, request.unwrap(), reply),
+                Status::Ok)) {
+        return;
+    }
+    auto const decoded = ours_examples_hello::decode_greet_reply(reply.message(), txid);
+    if (expect("Greet's reply", status_of(decoded), Status::Ok) &&
+        expect("Greet", decoded->status, Status::Ok) && decoded->response != HELLO_TEXT) {
+        auto _ = ort::debug_line({"routing_checks: hello answered \"", decoded->response, "\""});
+        fail("Greet: the answer is not what was sent");
+    }
+}
+
+/// A request hello has no method for: echo's EchoString. hello closes the
+/// connection without an epitaph, which reads as PeerClosed.
+auto check_hello_unknown(Uandle const connection) -> void {
+    std::array<u8, wire::MAX_MESSAGE_BYTES> bytes{};
+    auto const request = ours_examples_echo::encode_echo_string(
+        bytes, {.txid = next_txid(), .value = HELLO_TEXT});
+    if (!expect("a request of another protocol", status_of(request), Status::Ok)) {
+        return;
+    }
+    Incoming reply;
+    expect("hello's answer to a method it does not know",
+           call(connection, request.unwrap(), reply), Status::PeerClosed);
+}
+
+/// Stop on |connection|: hello answers Ok and exits, which closes the
+/// connection. Returns once it has.
+auto stop_hello(Uandle const connection) -> void {
+    auto const txid = next_txid();
+    std::array<u8, wire::STATUS_FORM_SIZE> bytes{};
+    auto const request = ours_examples_hello::encode_stop(bytes, {.txid = txid});
+    if (!expect("Stop", status_of(request), Status::Ok)) {
+        return;
+    }
+    Incoming reply;
+    if (!expect("Stop", call(connection, request.unwrap(), reply), Status::Ok) ||
+        !expect("Stop's reply",
+                status_of(ours_examples_hello::decode_stop_reply(reply.message(), txid)),
+                Status::Ok)) {
+        return;
+    }
+    Incoming closed;
+    expect("hello's end of the connection after Stop", closed.receive(connection),
+           Status::PeerClosed);
+}
+
+/// hello's first client: Greet, then a second client past hello's limit of
+/// one, then a request hello has no method for.
+auto check_hello_first_client() -> void {
+    auto const connection = connect_or_fail(HELLO);
+    if (connection == Uandle::Invalid) {
+        return;
+    }
+    check_hello_greet(connection);
+    expect("a second client of hello", connect_status(HELLO), Status::NoResource);
+    check_hello_unknown(connection);
+    auto _ = ours::handle_close(connection);
+}
+
+/// hello's next client, which stops it. The first client's entry is free by
+/// now: hello frees it before it closes the channel.
+auto check_hello_second_client() -> void {
+    auto const connection = connect_or_fail(HELLO);
+    if (connection == Uandle::Invalid) {
+        return;
+    }
+    stop_hello(connection);
+    auto _ = ours::handle_close(connection);
+}
+
+/// hello end to end, through root.cm's offer and hello's expose. Prints a
+/// line when nothing failed, so that a boot can require it.
+auto check_hello() -> void {
+    auto const failures_before = FAILURES;
+    check_hello_first_client();
+    check_hello_second_client();
+    if (FAILURES == failures_before) {
+        auto _ = ort::debug_line("routing_checks: hello checked");
+    }
+}
+
 /// The copy check_spawn() starts: the namespace it was handed is a clone of
 /// its parent's, read through once. Started by the shell instead (plan step
 /// 15), it gets the shell's namespace, which has /boot too. /denied is
@@ -902,6 +1000,7 @@ auto main(int argc, char **argv) -> int {
         check_spawn(job);
     }
     check_echo();
+    check_hello();
 
     if (FAILURES != 0) {
         char count[10];
```

### Checks of the chapter itself

- `python3 scripts/lint_docs.py --changed docs/guide/02-first-server.md .agents/tasks/server-guide-first-server/RESULT.md docs/repairs/261029/routing-checks.md docs/repairs/261029/echo.md`: no violations.
- Snippets: each `` `path:a-b` `` label is followed by a fence whose lines equal `git show HEAD:path` at `a-b`: 20 of 20. Prose citations: 27 of 27 inside their files. Links: 28 of 28 resolve, anchors included, against the working tree of `docs/guide/` as it was when checked (the chapters 01, 03, 04, 05 and 06 were being revised in parallel; the anchors that are new in that pass and that this chapter uses are `#where-a-servers-memory-comes-from` (01), `#the-callback-that-w4-to-w9-bind` and `#what-a-first-version-needs` (06)).
- Every line of a `text` fence is a line of a run above.
- No `.agents/` path appears except the verify script's and, once, its `SKILL.md`; no design-document numbers, change-record or `docs/repairs` pointers, `CLAUDE.md`, or other operating systems except macOS.

## Deviations from the design

- The contract asked each level to be 60 to 80 lines. Levels 0, 1, 4 and 5 are 39, 78, 75 and 34 lines; levels 2 and 3 are 144 and 110, because they carry the ideas the brief put in them (serving, memory, manifests, offer and use; the waiter, the server and the client). Each is split into two sub-sections (`The program` and `The route`; `The server` and `The client`) of about 75 lines. Cutting further would remove a step a reader needs.
- Level 5 does not "add the check to routing_checks": the checks are written in levels 2 to 4, because a level has to be verifiable when it is written. Level 5 adds the lines that prove the checks ran, and shows the boot fail when they do not.
- `hello` keeps echo's `Stop` from level 0, so that level 4 can end the server; the brief said "one method `Greet`". The chapter says so at level 0.
- The host test of the codec is left to chapter 3 (steps 6 and 7); the chapter links it and says why.
- `MAX_CLIENTS` is 1 in `hello`, not echo's 4, so that one connection and one more are enough to test the limit.
- The chapter uses "method number" and says once that the code calls it `ordinal`, as the revision rulings ask.

## Found along the way

Friction, with where the chapter handles it:

- Copied method numbers duplicate echo's, so `ordinals_test.py` fails twice for "unique" besides the two "hash" failures: level 0.
- A forgotten `#include <ort/main.hpp>` is a link error about `main`: level 1.
- The protocol's name is spelled in six places, and only some mismatches are caught by the build: the table "If it fails" at level 2. A name in the code that differs from the manifests gives a `NotFound` on the client and nothing in the root's log (the repair item `docs/repairs/261029/component-manifest.md` item 1 already records the silence).
- `grep` on `rel.log` without `-a` prints `Binary file ... matches`, because the log holds three NUL bytes: "Before you start". Chapter 5's `grep -n ... <dir>/rel.log` (the "Read the boot log" section) needs the `-a`.
- With a larger `MAX_CLIENTS` the busy check hangs and the boot fails by timeout with no `FAILED` line: level 4.
- The order of `init: hello exited with 0` and `routing_checks: hello checked` varies between boots: level 4.
- A call taken out of `main` is a build error (`-Wunused-function` as error), so the "remove the call" experiment of level 5 needs `static_cast<void>(&check_hello)`. The chapter says "take the call out"; a reader who deletes the call and keeps the function gets a build error and will not be misled, but the chapter could say it. Not changed.
- The tutorial has the reader edit `routing_checks.cpp`, a 900-line boot test, because no small client exists: repair item `docs/repairs/261029/echo.md` item 3.
- Already recorded elsewhere and met again: `ordinals_test.py` is run by nothing (`docs/repairs/261029/wire.md` item 2), so a wrong number passes a boot; level 0 says so.

What the other chapters and the README should now say:

- `README.md`: the chapter list entry `02-first-server.md (in preparation)` becomes a link; "Status of this guide" loses its first bullet and the paragraph that sends the reader to copy `examples/echo` by hand; "How to read it" says "to write your first server, go on to chapter 2"; the running-example section can end with "chapter 2 builds a server of your own the same way". Chapter 2 itself needs the build of "Build and boot the dev image" and links to it.
- `01-concepts.md` already links `02-first-server.md` for "builds the whole class"; its "Chapter 2 is in preparation" line in "Where to look next" (if still there) becomes a link.
- `03-protocols.md`: its recipe makes a library with the copier template and writes the header from nothing; chapter 2 copies echo's library and renames, which is shorter. Say in step 1 or 2 that a first protocol can be started by copying `examples/echo/protocol` as chapter 2 does, and that chapter 2 deletes the `tests` group and the `test/` directory, for which steps 6 and 7 are the way back.
- `05-testing-and-debugging.md`: "Read the boot log" greps `rel.log` without `-a`, which prints `Binary file ... matches`. In "Boot the dev image", the list of required lines is the dev image's own and does not include a reader's; say that a server of your own adds its lines there (chapter 2, level 5). The row of the table for "the client reads NotFound and the root logs nothing" (its row 5) is the case a reader meets when `HELLO_NAME` differs from the manifest.
- `06-checklist.md`: W22 to W26 and T5 can link level 2 and level 5 of chapter 2 as the worked case; W17 and W18 can link level 4.
- `04-tree-servers.md` needs nothing; chapter 2 links its "Choosing" and "Storage" sections.

## Not done

- A second clean worktree where the chapter's instructions are followed literally, by hand, was not made. The tree that booted was built by a script that applies the same instructions (a rename script for level 0, `examples/hello/hello_server.cpp` composed from line ranges of `echo_server.cpp` for levels 2 to 4, and Python edits for the manifests and the check); the final server file was compared against the renamed `echo_server.cpp` and differs from it only in the header comment and `MAX_CLIENTS`. The sentences of the chapter that describe the edits by prose (the stub class of level 2, the three bullets of level 4's client) were written from what the script did, not tried from the prose.
- A reply that cannot be written (level 4, third case) is not run by any check; the chapter says so.
- That a route to `hello` answers `NotFound` after it exited was not tried for `hello`; the chapter says "as for `echo`", whose check does it.
- Only the `rel` image was booted. No host other than the macOS machine of the project was tried.
- The `README.md`, `01`, `03`, `04`, `05` and `06` chapters were not edited, as instructed; the list above is what they should change.

## Revision 2

The second cold read (`.agents/tasks/server-guide-review-2/REVIEW.md`) found gaps in the chapter as committed in `26043afe`. Edited: `docs/guide/02-first-server.md` (538 to 662 lines, 4,535 to 6,017 words) and `docs/guide/README.md`. Nothing is committed or staged.

### How it was run

A second scratch worktree, `.worktrees/guide-hello2` (based on `b1d99016`; the files the chapter quotes were unchanged since `3cb0d8a5`), was made with `python3 scripts/new_worktree.py guide-hello2 HEAD`, the levels were applied to it by a script that applies the chapter's instructions (`lvl.py` in the session scratchpad, which holds the code the chapter shows as constants), and it was removed with its branch at the end. Fifteen boots, all `--only rel --out out/guide-hello2` except the last, and eight builds with `ninja` alone. A check showed that one six-row rename table, applied in order, reproduces byte for byte the library, every piece of `echo_server.cpp` that is pasted and both client copies (`check_echo_string`, `stop_echo`). The final `hello_server.cpp` differs from the renamed `echo_server.cpp` only in `MAX_CLIENTS` (1) and the file's header comment.

| Run | Result |
|---|---|
| Level 1 (cold build 54.9 s) | `rel PASS ... 6/6 required`; `init: started hello`, `hello: started`, `init: hello exited with 0` |
| Level 2a | `rel PASS ... 6/6 required`; `init: started hello` and no `init: hello exited`. Deps left out (ninja): `fatal error: 'ioservice/outgoing.hpp' file not found` |
| Level 2b | `rel PASS ... 6/6 required`; `init: started hello`, `routing_checks: passed`, no `routes nowhere` line naming hello |
| 2b, `NoResource` expected | `rel FAIL  2 FAILED line(s) ... 5/6 required`; `routing_checks: a connection to hello = NotSupported (expected NoResource) FAILED` |
| 2b, offer removed | `init: routing_checks's use of "ours.examples.hello.Hello" routes nowhere: NotFound at . (offer of "ours.examples.hello.Hello")` and `routing_checks: a connection to hello = NotFound (expected NotSupported) FAILED` |
| 2b, `HELLO_NAME` is `...Hallo` | the same `NotFound` `FAILED` line, and nothing from the root about hello |
| 2b, ninja only | `component_manifest: //examples/hello:hello.cm.manifest.expose[0].from: 'self' exposes 'ours.examples.hello.Helo', which `capabilities` does not declare`; `component_manifest: //kernel/products/image/dev:root.cm.manifest.offer[7].from: '#helo' names no child of this component` |
| Level 3 | `rel PASS ... 6/6 required`; `routing_checks: hello checked`, `routing_checks: passed` |
| 3, include forgotten (ninja) | `routing_checks.cpp:834:9: error: use of undeclared identifier 'ours_examples_hello'; did you mean 'ours_examples_echo'?` and five more errors from the same cause |
| 3, dependency forgotten (ninja) | `routing_checks.cpp:69:10: fatal error: 'ours_examples_hello/ours_examples_hello.hpp' file not found` |
| 3, `ECHO_STORAGE` left in `main` (ninja) | `hello_server.cpp:233:25: error: use of undeclared identifier 'ECHO_STORAGE'; did you mean 'HELLO_STORAGE'?` |
| 3, server answers `"goodbye"` | `rel FAIL`; `routing_checks: hello answered "goodbye"` and `routing_checks: Greet: the answer is not what was sent FAILED` |
| Level 4 | `rel PASS ... 6/6 required`; `hello: stopping, as a client asked`, `init: hello exited with 0`, `routing_checks: hello checked` |
| 4, the `Stop` branch back and `stop` missing (ninja) | `hello_server.cpp:176:17: error: use of undeclared identifier 'stop'` |
| 4, unknown method before the limit check | `rel FAIL  no "routing_checks: passed" within 60s ... 5/6 required`, no `FAILED` line |
| 4, `MAX_CLIENTS` 4 | the same line |
| Level 5, call replaced by `static_cast<void>(&check_hello)`, without the new lines | `rel PASS ... 6/6 required` |
| Level 5, the same with the new lines | `rel FAIL  no "init: hello exited with 0", "routing_checks: hello checked" within 60s ... 6/8 required` |
| Level 5, call deleted outright (ninja) | `routing_checks.cpp:907:6: error: unused function 'check_hello' [-Werror,-Wunused-function]` |
| Level 5, final, `--only rel` | `rel PASS ... 8/8 required`, host tests 148 passed |
| Level 5, final, no `--only` | `dbg PASS  shell finished at 5.5s · 44 passed, 4 skipped, 0 failed · 0 error line(s) · reached: end`, `rel PASS ... 8/8 required`, `host-tests PASS 148 passed` |

### Findings

- **H2 (short path).** Done. README: a section "The shortest path" before "What you will be able to do": its status, before you start and build-and-boot sections (865 words, "about 900"), `00-setup.md` (1,662), then chapter 2 with chapter 1 in pieces (first three sections 2,094; components and manifests 1,244; `/svc` and the outgoing directory, memory and epitaphs 1,856). Cumulative sizes counted by script: 7,947 words to the end of level 1 ("about 7,900"), 12,194 to level 3 ("about 12,200"), 13,407 to the end of level 5 ("about 13,400"). The README's "How to read it" says that chapter 2 is the way in and chapters 3 to 6 are reference; "Where to look next" points at the path. Chapter 2: "Before you start" now lists the pieces and their sizes, and levels 0, 1 and 2 open with a "Read now" line. The review estimated about 5,500 words to a booted `hello`; chapter 2 is now 6,017 words, so the count here is higher.
- **H3, the part that needs no decision.** Done. The line ranges of the program are given by content first ("from the comment `/// The Echo server:` above `class EchoServer` to the end of `echo_string()`"), with the numbers in parentheses as those of the checked commit; "Before you start" names `3cb0d8a5`, gives the command that prints the commit for the reader's checkout, and says to look for the quoted text if a number moved. "If it fails" tables now exist for levels 2a (a sentence), 3 and 4, from the failures above. Not done, as instructed: a finished copy of each level, example files in the tree.
- **M1.** Done. Level 3 says to add `#include <ours_examples_hello/ours_examples_hello.hpp>` after the echo include (line 68) and why it resolves; the table row about the missing file moved to level 3 and gained the undeclared-name error.
- **M2.** Done. The false sentence is replaced. Shown as code from the run: the stub class (level 2a), level 2b's `check_hello`, level 3's `check_hello`, `check_hello_unknown`, `check_hello_first_client`, `check_hello_second_client` and level 4's `check_hello`; each at most 12 lines. `check_hello_greet` and `stop_hello` are renamed copies of echo's functions and are described by the rename. The level 4 order is given by the order of the functions, and the 60 s hang is stated once, in one paragraph, with the failing order and `MAX_CLIENTS` above 1 as its causes; the table row points back to it.
- **M3.** Done. One six-row rename table in level 0 (rows for `ECHO_NAME`, `ECHO_STORAGE`, `ECHO_TEXT`, `Echo` and lower-case `echo` added) is applied to everything copied; level 2a lists exactly the names it changes in echo's program, including the three `echo:` strings, and the four that stay (`STOP`, `StopRequest`, `decode_stop`, `encode_stop_reply`).
- **M4.** Done. `## Level 2: a client can reach it` stays as the parent, with `### Level 2a: the program serves its outgoing directory` and `### Level 2b: the manifests and the route`. The links from `06-checklist.md` (`#level-2-a-client-can-reach-it`, `#level-5-gate-it`) still resolve.
- **M6.** Done as instructed, with no Level 6: the first item of "Where to look next" says the client of `hello` is a check in `routing_checks` because the tree has no small standalone client, and that a client of your own is the same exchange in a component of your own. It says that this has not been tried.
- **M7.** Done. Level 0: one sentence that chapter 3's recipe is for a protocol that is not a copy and is skipped for `hello`. README: chapter 3 is for a second method or a protocol of a different shape, in the chapter list and in "How to read it".
- **m1.** The colon after "with the signals it saw" is gone (the sentence now ends, and the pointer to chapter 1 follows). **m2.** The numbers of the deleted lines are said to be the original file's, and the comments are deleted last. **m3.** The source file is named separately from the header. **m4.** Level 0 tells the reader to rewrite the comments at the top of the header and of `BUILD.gn`. **m5.** "cause 10" and "cause 13", with the link to chapter 5's table. **m6 (README).** The skip list names "What a first version needs". **m7.** `<dir>` is defined. **m8.** Level 5 says to run without `--only rel`, and what the `dbg` line is, from the run above. **m9.** `serve` is explained as echo's name for the per-message function, and the table of echo's `Client` is called an entry. **m14.** `.cm` is expanded where it is first required.
- **Not in the list but in the review's "not said".** Added to "The places you will touch": the edits change the dev image for good, so work on a branch (`scripts/new_worktree.py`), and `hello` is not added to `examples/BUILD.gn`, which the run confirms.
- **Not done.** m10 to m13 and m15 belong to other chapters. T1 to T3 (breaking W6 and W9 on purpose, a server that is also a client, a method with another shape) are teaching additions the coordinator did not ask for.

### Checks

- `python3 scripts/lint_docs.py --changed` on the chapter, the README and this file: no violations.
- `check_guide_all.py`: no problem for the README; for chapter 2 it flags the eight blocks that are code you write and not in the tree (level 1's `main`, the stub, three `check_hello`s, `check_hello_unknown`, the two clients), each of which is found verbatim in the files of the run. The flags for chapters 0, 3 and 5 are the known ones.
- Snippets (19 labelled, at most 15 lines, verbatim at HEAD), 25 prose citations and 40 links with anchors: all resolve. Every `text` fence is a line of a run above, except the two path-shortened lines of the baseline block, which are real lines with `...` for the path.

## Revision 3

The maintainer decided option (b) for the review's finding H3: the finished result of the tutorial is committed as `examples/hello`, compiled by the default build and started by no image. Made in a scratch worktree, `.worktrees/guide-hello-tree`, left for cherry-picking: branch `guide-hello-tree`, commit `57ae6267` (`feat(examples): hello, the finished result of the developer's guide tutorial`), ten files, nothing under `kernel/products/image`. A second worktree from the same commit, used to test the chapter, is removed with its branch. The chapter and the README are edited in the main checkout and uncommitted.

### What the commit holds

- `examples/hello/`: `protocol/` (`BUILD.gn`, `.clang-format`, `.gitignore`, the header, the source; `Greet` and `Stop`; no `test/`, no `README.md`), `hello_server.cpp` and `BUILD.gn` (`user_executable("hello_server")`, `component_manifest("hello.cm")`, the user-toolchain assert).
- `BUILD.gn` (root): the default group names `//examples:hello` and `//examples/hello/protocol($host_toolchain)` after the echo codec's tests, with a comment.
- `examples/BUILD.gn`: a new `group("hello")` with the two user-toolchain labels; the existing `group("examples")` now names `echo_server` and `profile` in the user toolchain too and lists `:hello`.
- `docs/changes/261029/examples-hello.md`: the change record.

Two departures from the brief, both forced by GN and both in the change record. First, the root group names `//examples:hello` and not the two user-toolchain labels, because `hello_server` is visible to `//examples/*` and `hello.cm` to `//kernel/products/image/*` only, and the dev image must not change; so the group lives in `examples/`, and `hello.cm`'s `visibility` also lists `//examples/*` (as `examples/profile` does), which the chapter now tells the reader to write. Second, naming any target of `examples/BUILD.gn` evaluates the whole file, and its old group named `echo:echo_server` and `profile` without a toolchain, so the first boot stopped at `examples/profile must be built in a //build/toolchain/ours:user-* toolchain`; nothing had ever named that group. Its two labels carry the user toolchain now.

### How the files were made, and checked

`answer.py` (in the session scratchpad) derives `examples/hello` from `examples/echo` by the chapter's rules alone: the six-row rename table applied to every file, the `tests` group and its blank line cut from the protocol's `BUILD.gn`, the two method numbers computed from the selectors and written in groups of four, echo's `BUILD.gn` without its comments and its leading blank line, the program as echo's lines 27 to 300 with `usize{1}`, then `scripts/format_cpp.py` and `scripts/format_gn.py`. Before formatting its output is byte for byte what the Revision 2 run's hand-applied script wrote. Formatting matters: `echo_string` becomes the shorter `greet`, and clang-format then joins lines in seven places, so the committed text is the formatted one.

### Verification

- `python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/guide-hello-tree`, from a clean build directory: `dbg PASS` (44 passed, 4 skipped), `rel PASS` (41 passed, 8 skipped, 6/6 required), `host-tests PASS` (148 passed). The host-test count did not change: the finder runs the `testonly` executables of the host toolchain, `hello` adds none, and `gn ls --testonly=true --type=executable` lists 152 before and after.
- `gn desc out/guide-hello-tree //:default deps --all` reaches `//examples/hello:hello_server`, `//examples/hello:hello.cm` and the protocol library in the host and in the user toolchain; `ninja -t targets all` lists their outputs (`user-x86/obj/examples/hello/hello_server.image.elf`, `user-x86/gen/examples/hello/hello.cm`, `host/obj/examples/hello/protocol/src/protocol.ours_examples_hello.cpp.o`).
- `python3 system/lib/wire/test/ordinals_test.py`: `OK` with the new protocol in the tree.
- `clang-tidy -p out/guide-hello-tree` on `examples/hello/hello_server.cpp` and `examples/hello/protocol/src/ours_examples_hello.cpp`: no diagnostics (the repository's configuration enables six checks). `format_cpp.py --check` and `format_gn.py --check` are clean on every file added or changed.
- The literal run (a second worktree from the commit; `reader.py` in the scratchpad finds each edit by the text the chapter names and takes the reader-written code out of the chapter's own code blocks): `rm -r examples/hello` and the block of the root `BUILD.gn` deleted as the chapter says, then levels 0 to 5. Boots: level 1 `rel` 6/6 (`init: started hello`, `hello: started`, `init: hello exited with 0`); level 2a `rel` 6/6, `init: started hello`; level 2b `rel` 6/6, `routing_checks: passed`; level 3 `rel` 6/6, `routing_checks: hello checked`; level 4 `rel` 6/6 with `hello: stopping, as a client asked`, `init: hello exited with 0`, `routing_checks: hello checked`; at the end, after the formatters, the full command: `dbg PASS` (44 passed), `rel PASS` (41 passed, 8/8 required), `host-tests PASS` (148 passed). (a) The build did not break with the reader's own `examples/hello`: `examples/BUILD.gn` stays as it is and is never read once the root stops naming `//examples:hello`. (b) `git status examples/hello` and `git diff -- examples/hello` both printed nothing: the tutorial's result is the committed answer, byte for byte.

### What the literal run changed in the chapter

- Level 0: the `tests` group is deleted with the blank line above it (lines 37 to 42, not 38 to 42), or the protocol's `BUILD.gn` ends with a blank line and is not gn-formatted; the comments at the top of the header and `BUILD.gn` are to be left alone while following the chapter (they were "rewrite them"), since a rewrite cannot equal the finished copy; the two method numbers are given in the grouping the file uses.
- Level 1: the table of level 0 is applied to echo's `BUILD.gn` (replacing `echo_server` and `echo.cm` alone left `examples/echo` in the assert's message), the blank line the comments leave at the top is deleted, and the manifest's `visibility` also lists `//examples/*`.
- Level 4: a sentence says what the file now is (echo's from the first `#include` to the end of `main`, renamed, `MAX_CLIENTS` 1), so a reader can check it.
- Level 5: a formatting step (`scripts/format_cpp.py`, `scripts/format_gn.py`) and the comparison `git status examples/hello` / `git diff -- examples/hello`. One reader-written block (`check_hello_unknown`) was not clang-format clean and is now the formatted text.
- "Before you start": says the finished result is in the tree, how to set it aside (`rm -r examples/hello` and the block of the root `BUILD.gn`, with its first and last line), why, and that it is what to diff against; a sentence under the "If it fails" tables of levels 3 and 4 and an item in "Where to look next" say the same; the README's line about chapter 2 gains a clause; "Sources" names the formatters, `examples/hello` and the two `BUILD.gn` files.
- The command that prints the commit the line numbers were checked against is now `git log -1 --format=%h -- examples/echo system kernel/products .agents/skills/ours-verify/scripts`: with `examples/hello` committed, the one the revision rulings name (`... -- system examples tools scripts build kernel/products`) prints the answer's commit instead of `3cb0d8a5`. The "Checked against" lines of the chapters still say `3cb0d8a5`.
- I used `rm -r examples/hello` where the brief said `git rm -r`: after `git rm -r` the index no longer holds the finished copy, so `git diff -- examples/hello` has nothing to compare with, and `git status examples/hello` would show the deletion and the new files until the reader runs `git add`.

### Not verified

- The reader's instructions for the free-form parts (where a blank line goes between pasted pieces) were applied by one reader, a script; the chapter fixes the result at the end of level 4, so a different layout shows as a diff against the finished copy.
- The tutorial's edits to `routing_checks.cpp`, `routing_checks/BUILD.gn`, the dev image's manifest and `boot_check.py` are not in the finished copy and so cannot be compared; they were booted, not diffed.
- `ninja -n` after the final build was not run clean: the worktree's BUILD.gn files had been touched for the comparison, which makes ninja regenerate; the build itself passed and the boot used it.

## Revision 4

The Loop now tells a Waiter when it cannot arm its wait again (`57c17730`, `8c012c9c`: `loop::Waiter::on_rearm_failed(Status)`, with an empty default; echo's `Client`, `LoopServer::Binding`, `ResourceConnection` and `LauncherConnection` override it). `examples/hello` and the guide copy echo, so they follow. Uncommitted in the main checkout: the eight documents of `docs/guide/`, this file and `docs/changes/261029/examples-hello.md`. Committed in a scratch worktree, left for cherry-picking: branch `guide-sync`, commit `0d71493e` (`feat(examples): hello follows echo, a client is told when its wait cannot be armed again`), one file, `examples/hello/hello_server.cpp`, 19 lines added and 1 changed. A second worktree used for the literal run is removed with its branch.

### The answer

`answer.py` (session scratchpad) derived `examples/hello` again from the new `examples/echo/echo_server.cpp` by the chapter's rules (the six-row table, echo's first `#include` to the end of `main`, `usize{1}`, the formatters). Only `hello_server.cpp` differs from the committed copy, by echo's three hunks renamed (`Client`'s comment, `Client::on_rearm_failed()`, `HelloServer::rearm_failed()`); the rename table needed no row, since none of the new lines names echo.

- `boot_check.py --out out/guide-sync`: `dbg PASS` (44 passed, 4 skipped), `rel PASS` (41 passed, 8 skipped, 6/6 required), `host-tests PASS` (151 passed): the same counts as before the change.
- `ordinals_test.py`: `OK`. `format_cpp.py --check`: clean. `clang-tidy -p out/guide-sync` on `hello_server.cpp`: no diagnostic.

### The literal run

A second worktree from `0d71493e`; `reader.py` (scratchpad) sets `examples/hello` aside as the chapter says and applies levels 0 to 5 from the chapter's text, finding each edit by content and taking the reader-written code out of the chapter's blocks. Level 3: `rel` 6/6, `routing_checks: hello checked`. Level 4: `rel` 6/6 with `hello: stopping, as a client asked`, `init: hello exited with 0` and `routing_checks: hello checked`. After the formatters, the full command: `dbg PASS` (44), `rel PASS` (41, 8/8 required), `host-tests PASS` (151). `git status examples/hello` and `git diff -- examples/hello` printed nothing. The chapter needed no change for this; its line numbers did.

### The guide

- **Citations.** Every `path:N` of the eight documents (full cites, and the bare `:N` that follows one) was mapped from `3cb0d8a5`, the commit the guide was last checked against, to `HEAD` with `git diff -U0`, by `cites.py` (scratchpad). 106 citations of `examples/echo/echo_server.cpp`, `loop.hpp`, `loop_server.*`, `resource_server.cpp`, `launcher_server.cpp`, `realm.hpp`, `init.cpp`, `qemu_boot.py` and the ioservice fakes had moved with no change to the cited lines, and were rewritten; nine citations whose range covers changed lines, or whose size changed, were read and rewritten by hand (below). `snippets.py` then compared every fence with a cite beside it: all match, but one in chapter 3 (`cpp_interface.gni:15-18`), which did not before either. The text around every citation of the changed files was read too: the first cited line of each is in the table printed during the run and says what the sentence says.
- **01-concepts.md.** The `Waiter` snippet is the new class (12 lines, `loop.hpp:106-117`); the text says the class has two virtual functions and says when `on_rearm_failed` is called (only after `Rearm` when the Loop could not arm the wait again, after it closed the handle and freed the slot), what it is for, what the status is, that its default does nothing and why that is right for a waiter that only answers `Close`. The list of what `run` does for a packet has a fourth step. "Who closes the handle" mentions it, echo's `Client` is said to override it, and "Where this is stated" cites the new comment (`loop.hpp:63-89`).
- **02-first-server.md.** Level 3 names the new method in the declaration sentence and has one paragraph where `on_rearm_failed` is first met (what it is for, what not to touch, that the default does nothing, a link to W9 of the checklist). The class bullet lists `rearm_failed`. Every echo line number in the prose and the tables is the new one (the class 86-247 and 269-271, the `Stop` branch 218-221, `stop` 249-267, the storage 273-282, `main` 284-318, `EchoServer server` 301, `greet` 229-247). The command that prints the commit and the commit are `8c012c9c`.
- **04-tree-servers.md.** The `Binding` paragraph says that a failed re-arm releases the connection through `on_rearm_failed` (`loop_server.cpp:78-88`); "the steps above" is `loop_server.cpp:90-126`.
- **05-testing-and-debugging.md.** The fake kernel's list of rules has a sixth entry, a wait a test has made fail (`fail_next_wait_async()`, `fake_kernel.hpp:49-52`), and says it is how a test reaches a `Binding`'s `on_rearm_failed`.
- **06-checklist.md.** No item is added or renumbered. W9 ("Free your record of a client when you close it") gets the new obligation: when the Loop tells you it could not arm your wait again, free the record in `on_rearm_failed()`. Its Why says the callback has returned by then, that only `on_rearm_failed()` is left to free it, that it is called once after the slot is free and so must not touch the handle; its Where has echo's `rearm_failed` as a second snippet (`echo_server.cpp:178-184`). W5 says the Loop arms, and tells the waiter if it cannot. The "callback" section describes both functions. W3, W19 and the other citations of `loop.hpp` and `echo_server.cpp` are new numbers (W19 adds that the waiter stays where it is until `on_rearm_failed()` returns).
- **03-protocols.md, README.md, 00-setup.md.** Citation numbers only (chapter 3), and the footer.
- **"Checked against".** Every chapter and the README end with `8c012c9c` and 2026-10-30, the answer of `git log -1 --format=%h -- system examples tools scripts build kernel/products` in the main checkout today. Once the `guide-sync` commit is cherry-picked the same command answers with that commit, because it touches `examples`; the lines are then one commit behind, and nothing they cite changed in it. Chapter 2's own command (`... -- examples/echo system kernel/products .agents/skills/ours-verify/scripts`) is not affected.

### Checks

- `python3 scripts/lint_docs.py --changed` on the eight documents, this file and the change record: no violations.
- `check_guide_all.py`: only the known flags (the eight reader-written blocks of chapter 2, and one each in chapters 0, 3 and 5, which were flagged before this revision too).
- `verify_chapter.py` for chapter 2: 19 snippets verbatim at their cited lines, 25 prose citations, 41 links with anchors.

### Not verified

- That a citation was true at `3cb0d8a5` before it was moved: the mapping is only as good as the guide's last check. The ranges that moved contain the text the sentences say (the table printed in the run), which is as far as I took it.
- The failing path itself on the target: no program can provoke it (the loop-waiter-notify result says why), so the new paragraphs rest on the Loop's comment and its host tests.
- Chapters 4 and 5 describe the root's resource and launcher servers only through the numbers of the `NoResource` lines they cite; the commits that gave those servers `on_rearm_failed` did not change what the chapters say about them.
