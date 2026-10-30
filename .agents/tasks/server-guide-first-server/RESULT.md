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
