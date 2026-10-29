# component-manifest outstanding repairs

## 1. A provider that serves its protocol under another name than its manifest declares fails silently, as a NotFound on the client

**Severity**
Medium: wrong, but not reached today. Every provider in the tree (`examples/echo`) spells the name the same in both places, so the condition is a typo in a new provider, which is what the developer's guide expects its readers to write.

**Symptom**
Latent, cannot be triggered today. Were a provider's manifest to declare `protocol = "ours.examples.echo.Echo"` while its program passes another string to `ioservice::Outgoing::add_protocol()`, the build passes, the root routes the client's use successfully and logs nothing, and the client reads the epitaph `NotFound` on the channel it connected with. No line on the console names the provider, the protocol or the two spellings. The host test `system/lib/ioservice/test/outgoing_test.cpp:204-216` shows the provider's side of it: an Open of a name it does not serve answers `NotFound`.

**Cause**
The name is written twice, by hand, in two languages that no tool reads together: in the GN scope of `component_manifest()` (`examples/echo/BUILD.gn:39` and `:44`), and in C++ as a constant (`examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:46`), which the program passes to `add_protocol()` (`examples/echo/echo_server.cpp:268`). The manifest compiler (`system/component_manifest.py`) never sees the program's code, and the root forwards the Open fire-and-forget (`system/service/init/realm.cpp:809-824`), so it cannot tell whether the provider has a node of that name. The provider's `LoopServer` refuses the Open with an epitaph and logs nothing (`system/lib/ioservice/src/loop_server.cpp:163-169`).

**Change**
Not decided. The options: (a) log a line in `LoopServer`'s Refuse case through the delegate, as `write_failed()` does for a failed reply, so that a provider says "Open of an unserved name" with the name; the cost is a second hook on `Delegate`, and a line per refused Open, which a hostile client could repeat; (b) have the component manifest list the protocols a provider serves in a file that both the manifest compiler and the program read, as the root's built-ins already come from `system/service/init/builtins.json`; the cost is a generated header for every provider; (c) a boot check per provider, which is what `routing_checks` does for echo, and which only covers providers that have one. (a) is the smallest and helps whoever debugs it; (b) removes the cause.

**Reference**
None, this project's own design.

**Acceptance**
For (a): a host test in `outgoing_test.cpp` that adds a protocol under one name, opens another, and sees the delegate's hook called with the name. For (b): changing the name in only one of the two places fails the build.
