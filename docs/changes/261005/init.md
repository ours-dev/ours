# init changes

## Title: the root offers ours.resource.Irq and ours.resource.Ioport, and routing_checks takes a line and a port with them (2026-10-05)

**Files**
- `system/lib/ours.resource/include/ours_resource/ours_resource.hpp`, `src/ours_resource.cpp` — the three protocols as `ours_resource::Protocol` values, `DEBUG_PROTOCOL`, `IRQ_PROTOCOL` and `IOPORT_PROTOCOL`, each Get declared beside its selector; one set of functions that take the protocol, `encode_get()`, `decode_get()`, `encode_get_reply()` and `decode_get_reply()`, replaces Debug's four
- `system/lib/ours.resource/test/ours_resource_test.cpp` — every test runs on the three protocols, and a new one checks that each protocol refuses the others' requests and replies
- `system/lib/ours.resource/BUILD.gn`, `test/BUILD.gn`, `README.md` — they say there are three protocols
- `system/lib/wire/test/ordinals_test.py` — the scan must find `ours.resource/Irq.Get` and `ours.resource/Ioport.Get`
- `system/service/init/resource_server.hpp`, `resource_server.cpp` — renamed from `debug_resource_server.*`: `ResourceServer` serves the one protocol it is made for and answers each Get with a duplicate of the resource it is given
- `system/service/init/init.cpp` — three ResourceServers, for Debug, Irq and Ioport, each on its own storage and over the root's own resource, handed to the Realm
- `system/service/init/realm.hpp`, `realm.cpp` — BUILTINS gains the two protocols, `BuiltinServers` their servers, and `connect()` binds a route that ends at one to its server
- `system/service/init/pools.hpp` — `RESOURCE_SERVERS` and `MAX_RESOURCE_CONNECTIONS`, a pool per server, counted in `LOOP_SLOTS`; two comments that had gone stale (the number of startup handles, and which Gets clients keep)
- `system/service/init/BUILD.gn` — the renamed sources
- `system/service/init/test/routing_checks/BUILD.gn`, `routing_checks.cpp` — two more protocol uses; `get_resource()`, which the Debug check now goes through as well, `check_irq_resource()` and `check_ioport_resource()`; `call()` moved up so that `get_resource()` can use it
- `kernel/products/image/dev/BUILD.gn` — root.cm offers the two protocols to routing_checks and to no one else
- `examples/shell/shell.cpp` — Debug.Get through the functions that take the protocol
- `system/lib/component/include/component/route.hpp` — the comment that lists the root's built-ins
- `docs/repairs/261004/init.md` — item 4 counts the new Gets among what clients can keep
- `.agents/tasks/microkernel-interrupts/PLAN.md` — the files list names every file this step changed

**Symptom**
None; this is step 13 of the user-space interrupt plan (`.agents/tasks/microkernel-interrupts/PLAN.md`). Before it, the root took the roots of the interrupt lines and the I/O ports from userboot (step 9), logged `init: holding the IRQ and I/O port resources`, and gave them to no one. No component could make an interrupt object on a physical line or open an I/O port, so step 12's PS/2 driver had no way to the line and the ports it needs.

**Cause**
The objects:
- the IRQ root and the I/O port root: resources of kind Irq and IoPort with base 0 and size 0, which cover every line and every port. The kernel mints them, userboot hands them on, and the root keeps them, as it keeps VMEX (step 9);
- the root's built-ins: what `from: "parent"` means at the root, the fixed list BUILTINS in `realm.cpp` that `component::route()` looks a name up in once a walk reaches the root;
- a ResourceServer: one per ours.resource protocol, holding the resource it hands out and a pool of four connections, each on a Loop slot;
- routing_checks' `/svc/ours.resource.Irq`: a service node of its /svc, whose every Open the root routes again (D11).

The events of routing_checks' `get_resource("/svc/ours.resource.Irq", IRQ_PROTOCOL)`:
1. fsio writes Open("ours.resource.Irq", ProtocolService, server) on routing_checks' /svc and returns, since Open is one-way.
2. The root's slot for that /svc connection fires, and `Realm::connect()` routes the use: routing_checks' use, root.cm's offer from its parent, then BUILTINS, where `ours.resource.Irq` is a protocol. The route ends at a built-in.
3. `connect()` binds `server` as a fresh connection of the Irq ResourceServer.
4. routing_checks writes Get, 16 bytes with the ordinal of `ours.resource/Irq.Get`. The server's callback decodes it against the Irq protocol, duplicates the IRQ root with SameRights, and writes the Ok reply with the duplicate in it.
5. routing_checks takes the handle and closes the connection; the root's slot sees PEER_CLOSED and frees the entry.
6. routing_checks calls `interrupt_create(irq, 3, RemapIrq)`. The kernel checks that the resource covers [3, 4) (step 11), turns ISA 3 into its GSI, and takes the line; closing the interrupt destroys it, which lets the line go.

Why each piece is shaped the way it is:
- **Why a duplicate of the whole root (D5 a).** The alternatives were for the root to cut each driver an exclusive sub-resource from a static configuration (D5 b), which needs `RsrcFlag::Exclusive` and a range allocator that ours does not have, or to create the interrupt objects itself and hand out only those (D5 c), which would put device knowledge, which line and which trigger mode, into the root. With one driver, a sub-resource would protect it from nobody. The cost of (a) is that whoever holds the root can take any line that is free and open any port the kernel does not drive.
- **Why routing is the only gate, and what that asks of root.cm.** The root answers every Get that reaches its server without asking who sent it: there is no security policy (a non-goal of the component framework, `docs/design/261004/component-framework.md:49`). So the only thing that keeps the IRQ root from a component is that no offer leads to it, and root.cm offers the two protocols to routing_checks alone; the shell, which is offered Debug, is not offered these. The shell's start-up check on dev that it holds neither root as a startup handle stays true: a component reaches them only by a route.
- **Why one server class, made once per protocol.** The three protocols have one shape: closed, one method, an empty request, a reply with a status and one handle. Only the name, the ordinal and the resource differ, so those are what a ResourceServer is constructed with. Three copies of DebugResourceServer would be three copies of one callback, where every later fix to it, to the reply's I1 handling say, would have to be made three times. A single server for all three protocols was the other shape: one pool and one set of buffers, saving two storages of 4,448 bytes each, but each connection would then carry its protocol and the server a map from protocol to resource. The route already knows which protocol it reached when `connect()` binds the channel, so a server per protocol takes that decision once, at bind, and no message can change it.
- **Why the decoder takes the protocol.** The three Gets are the same 16 bytes but for the ordinal. `decode_get(message, protocol)` checks the ordinal against that protocol's own Get, so a connection speaks exactly one protocol: had `connect()` bound an Irq connection to the Ioport server, the server would refuse the request and close the connection, rather than hand out a resource the route never reached. The client's `decode_get_reply()` checks the same on the way back. The host test `ProtocolsRefuseEachOther` fixes both.
- **Why Debug's codec functions changed name.** `encode_debug_get()` and its three siblings would have become twelve functions, eight of them copies. One set taking the protocol is less to keep true, and the protocol a client speaks becomes something it names at each call, `ours_resource::DEBUG_PROTOCOL`, rather than something baked into a function's name. The shell's two calls follow. The constants are `*_PROTOCOL` rather than `DEBUG`, `IRQ` and `IOPORT`, since a bare `DEBUG` is the macro name a build is most likely to define.
- **Why four connections per server.** A client asks for its resource once, on a connection it closes after the reply, so a pool holds at most a few at a time; four is what Debug had. The Loop slots grow from `3 * 8 + 128 + 4 + 4 = 160` to `3 * 8 + 128 + 4 + 3 * 4 = 168`, under PORT_MAX_OBSERVERS, 1024.
- **Handle charges.** A Get's duplicate is made by the root, so it counts against the root's quota until the client closes it (`HANDLE_MAX_PER_PROCESS`). routing_checks closes both at once; a driver would keep one of each for as long as it runs. That is repair item 4 of `docs/repairs/261004/init.md`, which now names these Gets too.

**Change**
The codec, before:

```cpp
inline constexpr auto DEBUG_NAME = std::string_view{"ours.resource.Debug"};
inline constexpr auto DEBUG_GET = wire::Method{.selector = "ours.resource/Debug.Get", ...};
auto encode_debug_get(std::span<u8> bytes, DebugGetRequest const &request) -> ...;
auto decode_debug_get_reply(wire::Message message, u32 txid) -> ...;
```

and after, one protocol a value and the functions taking it:

```cpp
struct Protocol {
    std::string_view name;
    wire::Method get;
};
inline constexpr auto IRQ_PROTOCOL = Protocol{
    .name = "ours.resource.Irq",
    .get = wire::Method{.selector = "ours.resource/Irq.Get", .ordinal = 0x273c'3e02'af63'50e0,
                        .kind = wire::MethodKind::TwoWay},
};
auto encode_get(std::span<u8> bytes, Protocol const &protocol, GetRequest const &request) -> ...;
auto decode_get(wire::Message message, Protocol const &protocol) -> ...;
```

with `ours.resource/Ioport.Get` at `0x231b'c3b7'4044'47b5` and Debug's ordinal unchanged.

The root, before:

```cpp
init::DebugResourceServer debug_resource_server(debug_resource_storage, loop, debug);
```

and after, one server per protocol over the resource the root keeps for it:

```cpp
init::ResourceServer debug_resource_server(debug_resource_storage, loop, ours_resource::DEBUG_PROTOCOL, debug);
init::ResourceServer irq_resource_server(irq_resource_storage, loop, ours_resource::IRQ_PROTOCOL, irq);
init::ResourceServer ioport_resource_server(ioport_resource_storage, loop, ours_resource::IOPORT_PROTOCOL, ioport);
```

`Realm::connect()` binds a route that ends at one of them:

```cpp
} else if (routed->name == ours_resource::IRQ_PROTOCOL.name) {
    status = m_builtins.irq_resource->bind(channel);
} else if (routed->name == ours_resource::IOPORT_PROTOCOL.name) {
    status = m_builtins.ioport_resource->bind(channel);
}
```

root.cm gains one offer:

```text
{
  protocol = [ "ours.resource.Irq", "ours.resource.Ioport" ]
  from = "parent"
  to = "#routing_checks"
}
```

routing_checks uses both and checks, each failure a FAILED line:
- Irq.Get answers Ok with a handle that `interrupt_create(handle, 3, RemapIrq)` accepts, and the interrupt closes again;
- Ioport.Get answers Ok with a handle that `ioports_request(handle, 0x70, 1)` and `ioports_release(handle, 0x70, 1)` accept;
- Debug.Get answers as before, now through the same `get_resource()`.

Rejected:
- Three server classes, or three codecs: the same code three times, as above.
- One server for all three protocols: the shape described above, which trades two storages for a per-connection protocol and a map inside the server.
- Registering a built-in only when the root holds its resource, as component_manager registers IrqResource only `if let Some(...)` of its startup handle: BUILTINS is a constant list, and a root started without the resource still routes the protocol and answers each Get with the duplicate's failure, BadHandle, as Debug already does. The client is told why, where a missing built-in would read as a route that leads nowhere.
- Checking each resource's kind before serving it, as component_manager's `IrqResource::serve()` does at the start of every connection, closing the connection before any Get when the handle is not of the IRQ kind: the root takes each resource by its handle type, which userboot sets from what the kernel minted, and a resource of the wrong kind shows at the client's first use as BadType, which is the second mutation check below. Debug's server made the same choice in component-framework step 14.
- Listing `ours.resource.Ioport` on x86 alone, as component_manager registers IoportResource only under `#[cfg(target_arch = "x86_64")]`: BUILTINS is one constant list for every architecture, the kernel's userboot mints the I/O port root without asking which architecture it runs on (`kernel/lib/userabi/src/userboot.cpp`), and only x86 boots today. When another architecture boots, the root and the protocol that hands it out are left out there together (found at review: the record named the reference's x86-only registration without saying what ours does).
- routing_checks reading the RTC through the port it opened: whether a request opens a port is `ioport.test`'s and the dev.dbg_1 shell's to check. Here the point is that the route hands over a resource that works.

**Reference**
- Fuchsia component_manager at `51f05104c13`: `IrqResource::serve()` and `IoportResource::serve()`, each answering Get with `duplicate_handle(zx::Rights::SAME_RIGHTS)` after checking the resource's kind (`src/sys/component_manager/lib/builtins/src/irq_resource.rs:22-33`, `src/sys/component_manager/lib/builtins/src/ioport_resource.rs:22-35`); their registration in `BuiltinEnvironment::new()` with `add_builtin_protocol_if_enabled()`, only when the startup handle is there and, for IoportResource, only on x86 (`src/sys/component_manager/src/builtin_environment.rs:596-597`, `:634-648`); the protocols `fuchsia.kernel.IrqResource` and `fuchsia.kernel.IoportResource` (`sdk/fidl/fuchsia.kernel/irq-resource.fidl`, `sdk/fidl/fuchsia.kernel/ioport-resource.fidl`); the x86 board driver's use of both, the routed consumer step 12's driver stands in for (`src/devices/board/drivers/x86/meta/x86.cml:30-37`); and the security policy ours does not have, which lets only `/bootstrap` and the boot drivers reach the two (`src/security/policy/component_manager_policy_base.json5:82-101`).
- Zircon: `sys_interrupt_create()`, which takes an Irq resource for a physical line through `validate_resource_irq()` (`zircon/kernel/lib/syscalls/driver.cc:194-221`), and `sys_ioports_request()` (`:158-179`): the calls the two resources handed out here are for.
- Design: `.agents/tasks/microkernel-interrupts/DESIGN.md`, D5 and the root's built-ins under "Objects"; `docs/design/261004/component-framework.md`, "built-ins", D11, D19, D27, and the security policy among its non-goals (`:49`).

**Verification**
- `boot_check.py`: dbg PASS (34 passed, 2 skipped, 0 failed, 0 error lines), rel PASS (30 passed, 10 skipped, 0 failed, 3/3 required, 0 error lines), the same counts as before the change; dev's log holds `routing_checks: passed` and `init: routing_checks exited with 0`. The same again built into a fresh out dir, so no object left behind by the rename satisfies a link.
- Mutation checks, each made by hand on the rel image and reverted:
  - root.cm's new offer removed: the root logs `init: routing_checks's use of "ours.resource.Irq" routes nowhere: NotFound at . (offer of "ours.resource.Irq")`, and the same for Ioport, routing_checks prints `routing_checks: ours.resource/Irq.Get = NotFound (expected Ok) FAILED`, the same for Ioport, and `routing_checks: 2 check(s) FAILED`, and exits 1;
  - the two roots swapped where `init.cpp` builds the servers: `interrupt_create(the resource Irq.Get gave, ISA 3) = BadType`, and `ioports_request` and `ioports_release` the same, three FAILED lines and exit 1;
  - Irq's ordinal mistyped by one: `ordinals_test.py` fails, naming the header, the selector, and both values.
- Host tests: `ours_resource` 6 passed (5 before, each of them over Debug only), `route` 32 passed; `python3 system/lib/wire/test/ordinals_test.py`: OK, 5 tests.
- `ProtocolsRefuseEachOther` pairs every two protocols by name (found at review). It first skipped a pair whose ordinals were equal, so two protocols declared with one ordinal, which would accept each other's Get, passed it: with Irq's ordinal set to Debug's, a temporary mutation, all 6 tests passed and only `ordinals_test.py` failed. Under the same mutation the test now fails on the pair Irq and Debug.
- `llvm-nm -S` on init: `debug_resource_storage`, `irq_resource_storage` and `ioport_resource_storage` in `.bss`, 4,448 bytes each; `llvm-readelf -S`: no `.init_array`.
- clang-format clean on every changed line; clang-tidy reports nothing on `ours_resource.cpp`, `ours_resource_test.cpp` (beyond googletest's own headers), `resource_server.cpp`, `realm.cpp`, `init.cpp`, `routing_checks.cpp` and `shell.cpp` (beyond `bootfs/view.hpp`, which predates this change).

**Deliberately not done**
- An exclusive sub-resource per driver (D5 b): once there is more than one driver.
- A gate on the built-ins besides routing: the component framework has no security policy, a non-goal.
- The PS/2 driver that uses both protocols: step 12, next.
- `docs/design/261004/component-framework.md` is not revised: its "built-ins" row lists what that design was accepted with, and these two come from the interrupt design's D5, which names them.

**How it was found**
None, a planned step. The review of the step found one hole in a host test: `ProtocolsRefuseEachOther` skipped any pair of protocols whose ordinals were equal, the one mistake it is there to catch (Verification). It also found the record unclear on the reference twice: when component_manager checks a resource's kind, and what ours does where it registers IoportResource on x86 alone (Rejected).

**The user's prediction**
None.

**Three questions you should be able to answer**
- The shell on dev reaches the root's built-ins through its /svc just as routing_checks does. What stops it from getting the IRQ root, and what single change would hand it one?
- The three Gets are the same bytes but for the ordinal. If `Realm::connect()` bound an Irq connection to the Ioport server, what would routing_checks see on that connection, and which check in which function makes it so?
- Each Get answers with a duplicate made by the root. Whose handle quota pays for it, until when, and what would every other client see if one driver called Get in a loop without closing what it got?

## init serves the Sampling resource as ours.resource.Sampling (2026-10-05)

**Files**
- `system/lib/ours.resource/include/ours_resource/ours_resource.hpp` — `SAMPLING_PROTOCOL`, `ours.resource/Sampling.Get`
- `system/service/init/init.cpp`, `realm.hpp`, `realm.cpp` — init takes the Sampling startup handle and serves it with a fourth `ResourceServer`, listed among the built-ins and routed like the others
- `kernel/products/image/dev/BUILD.gn` — `root.cm` offers it to the shell and routing_checks
- `examples/shell/BUILD.gn` — `shell.cm` uses it
- `system/service/init/test/routing_checks/BUILD.gn`, `routing_checks.cpp` — uses it, and checks that `sampler_create` takes what `Get` returns (as first committed it created and closed a session; the next entry changes it to claim none)
- `.agents/tasks/prof-sampler-object/DESIGN.md`, `RESULT.md` — the open question moves on

**Symptom**
On dev no program could profile: userboot handed init the Sampling resource, and init never took it. Programs that need a capability got it, if at all, as a startup handle in a numbered slot, which the user wants programs not to know about.

**Cause**
When the resource was minted (step 3 of the sampler task), only dbg_1's shell took it; serving it on dev was left as an open question.

**Change**
The same shape as `ours.resource.Debug`: init takes the resource at start, a `ResourceServer` answers each `Get` on its protocol with a duplicate, and the protocol is a built-in that `root.cm` routes. Routing is the gate: a component can hold the resource only if its manifest uses the protocol and the root offers it. The shell is offered it now so that the programs it starts, which inherit its namespace, can reach it at `/svc/ours.resource.Sampling`; moving `bin/profile` onto that path is the next step.

**Reference**
Fuchsia's component_manager serves `fuchsia.kernel.SamplingResource`, which its cpu_profiler connects to for the resource.

**Verification**
`wire/test/ordinals_test.py` passes with the new ordinal (SHA-256 of the selector). `boot_check.py --out out/sampler`: dbg PASS (34 passed, 2 skipped), rel PASS (30 passed, 10 skipped, 6/6 required); on dev `routing_checks: passed`, which now includes connecting to `/svc/ours.resource.Sampling`, a `Get` answered Ok, `sampler_create` succeeding with the handle, and closing the session.

**Deliberately not done**
`bin/profile` still takes the resource as a startup handle; that is the next step.

**How it was found**
The user chose to have programs reach capabilities through the namespace rather than startup handles.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Which two things must both hold for a component to get the Sampling resource?
- Why is offering it to the shell enough for the programs the shell starts?
- How does routing_checks prove that what `Get` returned is the Sampling resource and not another?
