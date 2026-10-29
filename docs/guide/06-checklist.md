# Checklist: writing and reviewing a server

## What you will be able to do

- Check a server's design, code, tests and review against the rules the code itself lays down, in the order you meet them.
- Say, for each rule, why it exists and where it is stated or enforced, so you can look it up instead of taking it on trust.
- Tell which rules the build, the Loop or the root will catch for you, and which only a reader will catch.

This chapter does not teach. It is the list to keep open while you work, and each item points at the chapter or the source that does the teaching. A *server* here is a component with a program that answers requests on channels; the words below are the guide's, and each is linked to [01-concepts.md](01-concepts.md) where it first matters:

- A [handle](01-concepts.md#handles-and-channels) names a kernel object, and a channel is a two-ended pipe for messages.
- The Loop waits on many handles through one [port](01-concepts.md#ports-and-signals) and calls a callback when a handle's [signals](01-concepts.md#ports-and-signals) are raised; each handle it waits on sits in a [slot](01-concepts.md#the-loop-and-its-slots).
- An [epitaph](01-concepts.md#epitaphs) is the last message written on a channel before it is closed, saying why.
- A [manifest](01-concepts.md#components-and-manifests) declares what a component provides (a capability), needs (a use) and passes on (an offer from a parent, an expose from a child).
- The [`/svc` directory and the outgoing directory](01-concepts.md#svc-and-the-outgoing-directory) are how a client reaches a provider.

## The ten rules that matter most

1. Allocate nothing: a server has no heap, so every table is a fixed, `constinit` array ([W1](#w1-allocate-nothing), [W2](#w2-make-server-state-static-and-constinit)).
2. A callback does one unit of work and never waits ([W4](#w4-do-one-unit-of-work-per-callback), [W7](#w7-never-wait-inside-a-callback)).
3. A callback closes no handle and arms no wait: it answers Close or Rearm and the Loop does the rest ([W5](#w5-close-no-handle-and-arm-no-wait-in-a-callback)).
4. Answer Rearm only after you have read what raised the signal, or the server spins ([W6](#w6-answer-rearm-only-after-reading)).
5. Wait for Readable and PeerClosed together, and treat a callback without Readable as the client gone ([W8](#w8-wait-for-readable-and-peerclosed-together)).
6. Know who owns each handle at each moment, and close every handle you will not use, including those inside a message you reject ([D4](#d4-write-down-who-closes-every-handle), [W12](#w12-close-the-handles-of-a-message-you-do-not-use)).
7. Never leave a client waiting for an answer that cannot come: reply, close the connection, or send an epitaph ([W13](#w13-close-a-client-that-leaves-the-protocol), [W14](#w14-end-the-connection-when-a-reply-cannot-be-written), [W15](#w15-refuse-a-connection-you-cannot-take-with-an-epitaph)).
8. Size every table when you build, and decide what is answered when it is full ([D3](#d3-write-down-every-limit-and-what-it-answers), [W18](#w18-size-the-table-of-slots-and-check-it-at-build-time)).
9. Keep alive, and in place, everything the Loop and the libraries point at ([W19](#w19-keep-alive-everything-the-loop-points-at)).
10. Spell the protocol's name the same in the manifest, the code and the clients, and boot both images to prove the route ([W23](#w23-spell-the-protocols-name-the-same-everywhere), [T4](#t4-boot-both-images), [T5](#t5-make-the-boot-prove-your-route)).

## How to read an item

Every item has three parts, one line each:

- Rule: what to do, in one sentence.
- Why: what goes wrong without it.
- Where: the file and line where it is stated or enforced.

The Where line starts with one of two words. *Enforced* means something stops or reports a violation: a compile or build error, a status code at run time, or a boot check, and the line names it. *Convention* means nothing checks it; only a reader does, so these are the rules to look for hardest in a review. Where a rule is part of each, the line says which part.

The phases are in the order you meet them: design (D), writing (W), testing (T) and review (R). Review does not add rules; it turns the earlier ones into walks over a diff.

## Design

Decide these before you write code. They are cheap to decide and costly to change.

### D1. Choose the kind of server first

- Rule: Decide whether you are writing a protocol server (it answers a protocol of its own on each client's channel) or a tree server (it serves a tree of directories and files), before anything else.
- Why: The two are built from different parts, and a wrong first choice means a rewrite: a protocol server is your own handler and message codec behind `ioservice::Outgoing`, while a tree server is an `ioservice::Tree` with nodes you add, served by `Server` and `LoopServer`.
- Where: Convention. `system/lib/ioservice/README.md:7-10` lists the parts; [03-protocols.md](03-protocols.md) and [04-tree-servers.md](04-tree-servers.md) explain when each fits.

### D2. Plan for one thread that never waits

- Rule: Break the work into steps that each finish quickly and never wait for anything, because one thread runs every step for every client.
- Why: The Loop has no tasks, no timers and no extra threads, so steps run one at a time to completion; a slow step delays every client, and a step that waits for an answer from its own program waits forever.
- Where: Convention (nothing stops a program from calling something slow). Stated at `system/lib/loop/include/loop/loop.hpp:13-19`, and in the design's invariants I1 and I2 at `docs/design/261004/component-framework.md:190-191`, with the cost of a long step at `docs/design/261004/component-framework.md:204`.

### D3. Write down every limit and what it answers

- Rule: For every table and queue the server has, write down its size and what a client is told when it is full.
- Why: With no heap each table is a fixed array, and once it is full the only choices are to refuse the newcomer or leave it waiting; the kernel has limits of its own, and you meet them as status codes, not as crashes.
- Where: Convention for your own tables; the model is `examples/echo/echo_server.cpp:52-61` and `examples/echo/echo_server.cpp:141`. The limits you do not choose are these:

| Limit | Value | What you meet | Where |
|---|---|---|---|
| Bytes in one channel message | 4096 | `wire` refuses to build a bigger message (`InvArgs`) | `system/public/ours/syscall.hpp:43`, `system/lib/wire/include/wire/wire.hpp:190-194` |
| Handles in one channel message | 64 | the size your request-handle buffer needs | `system/public/ours/syscall.hpp:46` |
| Messages waiting on one end of a channel | 64 | a write answers `NoResource` and the message is consumed | `kernel/object/include/ours/object/channel_dispatcher.hpp:20-28`, `kernel/object/include/ours/object/channel_dispatcher.hpp:57-61` |
| Waits one port holds at once | 1024 | `Loop::add` can answer `NoResource` | `system/public/ours/port.hpp:167`, `system/lib/loop/include/loop/loop.hpp:175-179` |
| Handles one process has created | 1024 | creating one more answers `NoResource`; closing one gives the room back | `system/public/ours/syscall.hpp:59` |
| Protocols in one outgoing directory | 8 | `add_protocol` answers `NoResource` | `system/lib/ioservice/include/ioservice/outgoing.hpp:69`, `system/lib/ioservice/include/ioservice/outgoing.hpp:128` |
| Open connections to the outgoing directory | 4 | an Open past them is refused with the epitaph `NoResource` | `system/lib/ioservice/include/ioservice/outgoing.hpp:71-76`, `system/lib/ioservice/src/server.cpp:217-222`, `system/lib/ioservice/src/server.cpp:529-531` |
| Nodes in a tree | the storage you give | an add answers `NoResource` | `system/lib/ioservice/include/ioservice/tree.hpp:88-90` |
| Bytes in one debug write | 256 | a longer line goes out in several writes and can interleave with another program's | `system/public/ours/syscall.hpp:69`, `system/lib/ort/include/ort/debug.hpp:32-39` |

### D4. Write down who closes every handle

- Rule: For every handle the server ever holds, write down who owns it at each moment and who closes it.
- Why: A handle that is never closed keeps its object alive, and a channel end you forgot to close means the client's end never sees PeerClosed; a handle closed by the wrong party leaves the Loop waiting on something that is gone.
- Where: Convention. The hand-over points the libraries state are in this table:

| Handle | Whose | Closed by | Where |
|---|---|---|---|
| The handle you give `Loop::add` | the Loop's if `add` answers Ok, yours if it fails | the Loop, when the slot's callback answers Close | `system/lib/loop/include/loop/loop.hpp:202-207` |
| The port you give `Loop` | the Loop's from then on | the Loop's destructor | `system/lib/loop/include/loop/loop.hpp:181`, `system/lib/loop/include/loop/loop.hpp:189` |
| The DirectoryRequest handle you give `Outgoing::serve` | the Loop's on Ok, yours on failure | the Loop, as above | `system/lib/ioservice/include/ioservice/outgoing.hpp:138-141` |
| A client's channel handed to `Handler::connect` | yours from the call | you, or the Loop once you `add` it | `system/lib/ioservice/include/ioservice/outgoing.hpp:52-54` |
| Handles that arrive in a message | yours | you | `examples/echo/echo_server.cpp:172-191` |

### D5. Decide that your protocol is closed

- Rule: Decide that a message with a method you do not know, or one your decoder refuses, closes the connection and is not answered.
- Why: A peer that sends such a message is not speaking your protocol, no answer would help it, and the header has no bit that marks a call as one an unknown method may be answered for; the peer learns by seeing the channel close.
- Where: Convention for your own server; `ioservice` does it for the file protocol (`system/lib/ioservice/include/ioservice/server.hpp:83-90`), and `examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:39-40` states it for Echo. [03-protocols.md](03-protocols.md#closed-protocols) explains the rule.

### D6. Decide how the server ends

- Rule: Decide how your server stops (never, when a client asks, or on an error it cannot recover from), and expect that nothing will start it again.
- Why: The root marks a component that has exited and never starts it again, and it has no call to stop or kill one; a later route to it answers NotFound, and connections already open see PeerClosed.
- Where: Enforced by the root: `system/service/init/realm.cpp:782-785` answers NotFound for an exited provider, and `system/service/init/realm.cpp:852-855` marks it. The decision is stated in `docs/design/261004/component-framework.md:49` and `docs/design/261004/component-framework.md:392`. How a program stops is Convention (W20).

## Writing

The items run in this order: storage (W1 to W3), the callback (W4 to W9), messages (W10 to W15), ownership (W16, W17), limits and lifetimes (W18, W19), leaving (W20, W21), manifest and routing (W22 to W27) and, for a tree server, W28 and W29. The snippets are copied from `examples/echo`, the protocol server that runs in the dev image.

### W1. Allocate nothing

- Rule: Do not use `new`, `malloc` or any type that allocates, such as `std::vector`.
- Why: A user program has no heap: the runtime defines no `operator new` at all, and its `operator delete` traps on any pointer that is not null.
- Where: Enforced at link time and at run time: `system/lib/ort/src/cpp_dependencies.cpp:27-35` says an accidental `new` stays a link error that names the line, and `system/lib/ort/src/cpp_dependencies.cpp:16-24` traps a delete of a real pointer.

### W2. Make server state static and constinit

- Rule: Keep the server's tables in namespace-scope `constinit` variables, and put types with destructors in `ustl::LazyInit`, which `main()` builds in place.
- Why: Nothing runs the constructors of a program's global objects, so a global that needs one would silently start out as zeros; `constinit` makes the compiler reject such a global, and keeping the tables off the stack matters because the echo server's state is about 8 KiB.
- Where: Enforced by the compiler for `constinit`; that you use it is Convention. The reason is given at `system/lib/ort/src/start.cpp:41-45`, and `examples/echo/echo_server.cpp:238-243` explains the `LazyInit`. The echo server's storage:

```cpp
constinit ustl::LazyInit<ioservice::Outgoing::Storage> OUTGOING_STORAGE;
constinit ustl::LazyInit<EchoServer::Storage> ECHO_STORAGE;
constinit std::array<loop::Slot, SLOTS> SLOT_STORAGE{};
```

That is `examples/echo/echo_server.cpp:244-246`. The size note is at `examples/echo/echo_server.cpp:98-99`.

### W3. Give a base class no virtual destructor

- Rule: Give a class you derive handlers from a protected, non-virtual destructor, as `ioservice::Handler` does.
- Why: Nothing is ever deleted, since there is no heap, so a virtual destructor only adds a deleting destructor that calls `operator delete`, which traps on a real pointer.
- Where: Convention. `system/lib/ioservice/include/ioservice/outgoing.hpp:46-49` and `system/lib/ioservice/include/ioservice/outgoing.hpp:57-61` do it for `Handler`; `system/lib/ort/src/cpp_dependencies.cpp:27-35` says why `operator delete` exists at all.

### W4. Do one unit of work per callback

- Rule: In one callback do one unit of work, such as reading one message or reporting one exit, and answer.
- Why: A client with more messages queued is not called again at once; its next wait queues behind everyone else's, so a busy client is served in turns instead of starving the rest.
- Where: Convention for the callback; the Loop's side is Enforced. The rule is stated at `system/lib/loop/include/loop/loop.hpp:50` and as invariant I4 at `system/lib/loop/include/loop/loop.hpp:291-294`.

### W5. Close no handle and arm no wait in a callback

- Rule: A callback never closes its own handle (it answers Close), never closes another slot's handle, and never arms a wait (it answers Rearm); it may `add()` new slots.
- Why: The Loop closes a slot's handle only after the callback returns, when the slot's one wait has fired and its packet is taken, so nothing of the slot is left on the port; the Loop cannot withdraw a packet that is already queued, and it keeps at most one live wait per slot.
- Where: Convention (nothing stops a callback from calling `handle_close`). Stated at `system/lib/loop/include/loop/loop.hpp:51-54` and `system/lib/loop/include/loop/loop.hpp:214-215`, with the reasons at `system/lib/loop/include/loop/loop.hpp:286-290`, `system/lib/loop/include/loop/loop.hpp:295-296` and `system/lib/loop/include/loop/loop.hpp:298-306`.

### W6. Answer Rearm only after reading

- Rule: Answer Rearm only once the callback has consumed what raised the signal.
- Why: The wait is level-triggered, so it fires again as long as the signal is still raised; a callback that rearms with its message unread, or with only PeerClosed raised (which stays raised), is called again on every turn, and the server spins on the one thread's CPU.
- Where: Convention (nothing detects a spin). Stated at `system/lib/loop/include/loop/loop.hpp:55-61`.

### W7. Never wait inside a callback

- Rule: Never wait inside a callback or inside a function it calls: write replies and move on.
- Why: A callback that waits for a reply from its own program blocks the very thread that must read it, and one that waits for anyone else holds every other client back; a write to a full channel fails at once, so nothing needs to wait for room.
- Where: Convention. `system/lib/ioservice/include/ioservice/outgoing.hpp:52-54` and `system/lib/ioservice/include/ioservice/loop_server.hpp:45-47` say a function called from a callback "must not wait"; the design states it as I1 and I2 at `docs/design/261004/component-framework.md:190-191`, and the write that does not wait at `kernel/object/include/ours/object/channel_dispatcher.hpp:57-61`.

### W8. Wait for Readable and PeerClosed together

- Rule: Wait for Readable and PeerClosed together, read whenever Readable is raised, and treat a callback without Readable as the client gone.
- Why: A client that leaves with nothing left to read raises only PeerClosed, so a server that asked only for Readable never learns it left and its slot, handle and table entry stay for good; but a closed channel still delivers its last messages first, so a callback with both signals must still read.
- Where: Convention. `examples/echo/echo_server.cpp:132` asks for both and `examples/echo/echo_server.cpp:154-156` takes the missing Readable as the end; `system/lib/ioservice/src/loop_server.cpp:15` and `system/lib/ioservice/src/loop_server.cpp:87-90` do the same, and the channel's rules are at `kernel/object/include/ours/object/channel_dispatcher.hpp:40` and `kernel/object/include/ours/object/channel_dispatcher.hpp:67-69`.

```cpp
        if (!(observed & Signals::Readable)) {
            return release(client);
        }
```

That is `examples/echo/echo_server.cpp:154-156`.

### W9. Free your record of a client when you close it

- Rule: When a callback answers Close, free your own record of that client in the same callback.
- Why: The Loop frees the slot and closes the handle after the callback returns, but your table entry is yours to free, and the callback is the one place that knows the client is gone; an entry never freed makes the table one client smaller for good.
- Where: Convention. The echo server's one exit for a finished client:

```cpp
    /// Frees |client|'s entry; its slot closes the channel, by answering
    /// Close.
    static auto release(Client &client) -> loop::Action {
        client.channel = Uandle::Invalid;
        return loop::Action::Close;
    }
```

That is `examples/echo/echo_server.cpp:144-149`; `examples/echo/echo_server.cpp:127-130` is the search for a free entry that depends on it.

### W10. Read one message into buffers that hold the largest

- Rule: Read exactly one message per callback, into request buffers that hold the largest message a channel carries, in bytes and in handles.
- Why: A message that does not fit stays at the head of the queue and the read answers BufferTooSmall, so the same message fails every time and the connection can never be served again.
- Where: Convention; the echo server sizes its arrays with the wire constants. Stated at `system/lib/ioservice/include/ioservice/loop_server.hpp:111-116`, `examples/echo/echo_server.cpp:103-107` and `kernel/object/include/ours/object/channel_dispatcher.hpp:69-72`.

### W11. Finish with a decoded request before the callback returns

- Rule: Use a decoded request before the callback returns, and build the reply in a different buffer from the one you decoded from.
- Why: A decoded string or byte span is a view into the request buffer, not a copy; one buffer serves every callback, so the next message overwrites it, and a reply built in the same buffer would overwrite the bytes it is copying from.
- Where: Convention. `examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:69-73` says the decoded value views the message's bytes, and `examples/echo/echo_server.cpp:197-198` keeps the two buffers apart.

### W12. Close the handles of a message you do not use

- Rule: Close every handle a message carried that you do not use, including those in a message you reject.
- Why: A handle that arrives in a message is in your handle table; if you never close it the object it names stays alive, and the sender's other end never sees PeerClosed.
- Where: Convention (the `ioservice` code does it for you at `system/lib/ioservice/src/loop_server.cpp:134-137`). The echo server:

```cpp
        for (auto const handle: message.handles) {
            auto _ = ours::handle_close(handle);
        }
        return release(client);
```

That is `examples/echo/echo_server.cpp:188-191`.

### W13. Close a client that leaves the protocol

- Rule: When a message has a method you do not know, or your decoder refuses it, close the connection and send nothing.
- Why: The peer is not speaking your protocol, so no answer would help it; it learns from the close.
- Where: Convention for your own server, stated in the comment below and at `examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:39-40`; the file protocol's server does the same (`system/lib/ioservice/src/server.cpp:561-565`). The rule is explained in [03-protocols.md](03-protocols.md#closed-protocols).

```cpp
        // A closed protocol: an ordinal it does not know, or a message its
        // decoder refuses, closes the connection without an epitaph, as
        // <ours_examples_echo/ours_examples_echo.hpp> lays down. Neither
        // method takes a handle, so any the message brought are closed with
        // it.
```

That is `examples/echo/echo_server.cpp:172-176`.

### W14. End the connection when a reply cannot be written

- Rule: When a reply cannot be written, end that client's connection.
- Why: A write fails when the client is gone (PeerClosed) or its queue is full (NoResource), and nothing waits for room, so a reply that was not written will never be written and the client would wait for it for good.
- Where: Convention. The echo server, and `system/lib/ioservice/src/loop_server.cpp:115-125` for the file protocol:

```cpp
        // A reply that cannot be written leaves its client waiting for good,
        // so the connection goes with it.
        if (auto const written = write(client.channel, reply.unwrap()); written != Status::Ok) {
            return release(client);
        }
```

That is `examples/echo/echo_server.cpp:206-210`. The one reply that may be dropped without ending anything is the last one before the program exits (`examples/echo/echo_server.cpp:214-216`).

### W15. Refuse a connection you cannot take with an epitaph

- Rule: When you cannot take a new connection, write an epitaph with the reason on its channel, then close the channel.
- Why: The client has already sent its Open and expects a connection; the epitaph tells it why it was refused (NoResource, say) instead of leaving it to guess from a bare PeerClosed.
- Where: Convention. The echo server uses it for a full client table (`examples/echo/echo_server.cpp:141`) and for a failed `add` (`examples/echo/echo_server.cpp:137`); the epitaph's form is `system/lib/wire/include/wire/wire.hpp:35-36`.

```cpp
auto close_with_epitaph(Uandle const channel, Status const status) -> void {
    std::array<u8, wire::STATUS_FORM_SIZE> bytes{};
    if (auto const epitaph = wire::encode_epitaph(bytes, status); epitaph.is_ok()) {
        // The client may be gone already; the channel closes either way.
        auto _ = write(channel, epitaph.unwrap());
    }
    auto _ = ours::handle_close(channel);
}
```

That is `examples/echo/echo_server.cpp:77-84`.

### W16. After a failed add the handle is still yours

- Rule: When `Loop::add` fails, the handle is still yours: send an epitaph if the handle is a client's, and close it.
- Why: On success the handle belongs to the Loop, but on failure nothing changed, so if you do nothing the handle leaks and its client waits.
- Where: Convention (`add` is `[[nodiscard]]`, so ignoring its result draws a warning). Stated at `system/lib/loop/include/loop/loop.hpp:202-207`; the echo server does it at `examples/echo/echo_server.cpp:132-138`.

### W17. A channel handed to connect is yours to serve or close

- Rule: A channel handed to `Handler::connect` is yours from the call: serve it, usually by adding it to the Loop, or close it.
- Why: The library has given the channel up and nobody else will close it, so a handler that returns without doing either leaks it and its client waits.
- Where: Convention. Stated at `system/lib/ioservice/include/ioservice/outgoing.hpp:52-54` and, for the lower-level hook, at `system/lib/ioservice/include/ioservice/loop_server.hpp:54-57`.

### W18. Size the table of slots and check it at build time

- Rule: Give the Loop one slot for every handle that can wait at once, never more than `PORT_MAX_OBSERVERS`, and say so with a `static_assert`.
- Why: Each slot in use holds one live wait on the port and the port allows 1024; a larger table could meet NoResource from the port in a place where you planned no answer.
- Where: Enforced only by the `static_assert` you write; without one, `add()` answers NoResource at run time (`system/lib/loop/include/loop/loop.hpp:210`). Stated at `system/lib/loop/include/loop/loop.hpp:175-179`. The echo server counts one slot per client and one per connection to its outgoing directory:

```cpp
/// How many clients may be connected at once. One is all dev's routing
/// checks use; a client past them is refused with NoResource.
constexpr auto MAX_CLIENTS = usize{4};

/// The Loop's slots: one per connection to the outgoing directory, one per
/// client.
constexpr auto SLOTS = ioservice::Outgoing::MAX_CONNECTIONS + MAX_CLIENTS;

// Each slot in use holds one live wait on the port (<loop/loop.hpp>).
static_assert(SLOTS <= ours::PORT_MAX_OBSERVERS);
```

That is `examples/echo/echo_server.cpp:52-61`.

### W19. Keep alive everything the Loop points at

- Rule: Keep alive, and in place, everything the Loop and the libraries hold a pointer or a view to, for as long as they may use it.
- Why: None of them copies what it is given, because copying needs a heap, so a pointer into a stack frame that has returned, or into an object that has moved, is read later by the Loop.
- Where: Convention, except that copying and moving the library objects is Enforced: they have deleted copy constructors. What must outlive what:
  - the table of slots outlives the Loop (`system/lib/loop/include/loop/loop.hpp:172-173`);
  - the object a slot calls into stays where it is from `add()` until the slot is freed (`system/lib/loop/include/loop/loop.hpp:63-69`);
  - the `Outgoing` storage and the Loop outlive the `Outgoing`, and the protocol name (kept as a view) and the handler (kept by address) outlive the directory (`system/lib/ioservice/include/ioservice/outgoing.hpp:113`, `system/lib/ioservice/include/ioservice/outgoing.hpp:122-124`);
  - a tree's node names, and a file node's VMO and bytes, outlive the tree (`system/lib/ioservice/include/ioservice/tree.hpp:47-48`, `system/lib/ioservice/include/ioservice/tree.hpp:115-116`).

The deleted copies are at `system/lib/loop/include/loop/loop.hpp:193-196`, `system/lib/ioservice/include/ioservice/outgoing.hpp:117-118` and `system/lib/ioservice/include/ioservice/tree.hpp:102-103`. In the echo server the long-lived objects are statics (`examples/echo/echo_server.cpp:244-246`) or locals of `main` that never return, because `run()` does not (`examples/echo/echo_server.cpp:264-266`, `examples/echo/echo_server.cpp:281`).

### W20. Leave by exiting, and say why

- Rule: To stop, end the process from inside the callback with `ours::process_exit`, answering the client first; when you stop because something failed, log why and return a non-zero code from `main`.
- Why: The Loop has no quit and `run()` returns only when the port itself fails, so a server that should stop must end its process; everything the process holds closes as it dies, and the root logs the exit code, which is how a failed start is found.
- Where: Convention for what you choose; Enforced that the root logs `init: <name> exited with <code>` (`system/service/init/realm.cpp:849-850`). `system/lib/loop/include/loop/loop.hpp:258-259` says `run()` never returns Ok. The echo server's Stop:

```cpp
    [[noreturn]]
    auto stop(Client const &client, ours_examples_echo::StopRequest const &request) -> void {
        if (auto const reply = ours_examples_echo::encode_stop_reply(m_storage->reply_bytes,
                                                                     request.txid, Status::Ok);
            reply.is_ok()) {
            auto _ = write(client.channel, reply.unwrap());
        }
        auto _ = ort::debug_line("echo: stopping, as a client asked");
        ours::process_exit(0);
    }
```

That is `examples/echo/echo_server.cpp:223-232`, explained at `examples/echo/echo_server.cpp:214-222`. After the exit, later routes to the server answer NotFound (`system/service/init/realm.cpp:782-785`) and open connections see PeerClosed.

### W21. Keep each log line short

- Rule: Log every failure you cannot answer, in one line of at most 256 bytes that starts with the program's name.
- Why: The console is the only place a failed server speaks; a line longer than one debug write goes out in several and can interleave with another program's, and the name says whose line it is.
- Where: Convention. `examples/echo/echo_server.cpp:63-66` is the pattern, and `system/lib/ort/include/ort/debug.hpp:32-39` and `system/public/ours/syscall.hpp:69` give the limit.

### W22. Declare the protocol and expose it

- Rule: List the protocol under `capabilities` and `expose` it from `self`, or no client can be routed to it.
- Why: The root finds a provider by walking from the client's use up through offers and down through exposes to a declared capability; a missing declaration or expose ends the walk in NotFound.
- Where: Enforced: the build refuses an expose of an undeclared capability (`system/component_manifest.py:664-670`), and a route that ends early is logged by the root (`system/service/init/realm.cpp:194-203`). The echo server's manifest:

```gn
  manifest = {
    capabilities = [
      {
        protocol = "ours.examples.echo.Echo"
      },
    ]
    expose = [
      {
        protocol = "ours.examples.echo.Echo"
        from = "self"
      },
    ]
  }
```

That is `examples/echo/BUILD.gn:36-48`.

### W23. Spell the protocol's name the same everywhere

- Rule: Spell the protocol's name the same in the manifest's `capabilities`, in the code that passes it to `add_protocol`, and in every client's `use`, using only the characters a name allows.
- Why: The root forwards an Open under the capability's declared name, and the program's outgoing directory has an entry only for the name it added; if they differ, a good route ends at "no such entry" and the client reads the epitaph NotFound.
- Where: The character set and the length are Enforced at build time: a letter, digit or underscore, then those or `-` or `.`, at most 100 bytes (`system/component_manifest.py:227-238`). That the names match is Convention: the code keeps its name in one constant (`examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:42-46`) but a GN file cannot read it, so the manifest repeats it (`examples/echo/BUILD.gn:39`). The symptom is pinned by the host test at `system/lib/ioservice/test/outgoing_test.cpp:204-216`.

### W24. State the type where a rename sits between

- Rule: When an `offer` renames the protocol with `as`, state the protocol's `type` on the client's `use`, naming the protocol the provider declared.
- Why: A rename changes the name and not the protocol, so the name alone no longer says which protocol a route ends at; without the type, a client could be routed to a different protocol under the same name, and would learn it only as a bare PeerClosed when the server closes on an unknown method.
- Where: Enforced three times, in this order: the build (`system/component_manifest.py:1214-1233`), the root on the first Open (`system/service/init/realm.cpp:194-199`, the epitaph BadType) and, last, the closed protocol itself. A `type` goes with one protocol, not a list (`system/component_manifest.py:343-349`), and an `offer` or `expose` takes none (`system/component_manifest.py:563-568`, `system/component_manifest.py:655-660`). The design is `docs/design/261029/protocol-identity.md:38-41`.

The root's offer renames Echo, and the client's use says what protocol it means:

```gn
      {
        protocol = "ours.examples.echo.Echo"
        from = "#echo"
        to = "#routing_checks"
        as = "ours.test.Echo"
      },
```

That is `kernel/products/image/dev/BUILD.gn:186-191`.

```gn
      {
        protocol = "ours.test.Echo"
        type = "ours.examples.echo.Echo"
      },
```

That is `system/service/init/test/routing_checks/BUILD.gn:85-88`. A mismatch the build finds reads like this:

```text
error: //kernel/products/image/dev:root.cm: #pc-ps2 uses protocol "ours.resource.Irq" (type ours.resource.Irq),
       but the route ends at the root's built-in "ours.process.Launcher" (type ours.process.Launcher):
         use    #pc-ps2 "ours.resource.Irq"
         offer  root.cm "ours.process.Launcher" from parent to #pc-ps2 as "ours.resource.Irq"
       Offer a capability of type "ours.resource.Irq" under that name, or give the use the type it means.
```

That is `system/test_component_manifest.py:749-753`.

### W25. Put the component in the image and offer it

- Rule: Add the component as a child of the image's root manifest with `startup = "eager"` written out, and have the root manifest offer the protocol to each component that uses it.
- Why: A component exists only if the root manifest names it, and a client reaches it only through an offer; a use that nobody offers answers NotFound at the root.
- Where: Enforced: `startup` must be written out as eager (`system/component_manifest.py:450-452`), and an unoffered use answers NotFound, which the on-target test `routing_checks` checks (`system/service/init/test/routing_checks/routing_checks.cpp:525-541`). The dev image's children are `kernel/products/image/dev/BUILD.gn:88-109`.

### W26. Build the program under bin in a user toolchain

- Rule: Build the program with `user_executable()`, which installs it under `bin/`, and make its `BUILD.gn` fail outside a user toolchain, as the examples do.
- Why: The root makes only the files under `bin/` executable when it reads the boot image, and a user program is built in a user toolchain, which the example's `BUILD.gn` asserts.
- Where: Enforced: the manifest compiler refuses a program installed anywhere else (`system/component_manifest.py:411-415`), and `examples/echo/BUILD.gn:13-15` asserts the toolchain.

### W27. Serve the outgoing directory first

- Rule: Serve the outgoing directory before you do anything slow, and keep serving it for as long as you run.
- Why: The root forwards each client's Open into your outgoing directory and does not wait; Opens queue in the channel, which holds 64 messages, and past that a forward fails and the client sees PeerClosed with no reason.
- Where: Convention. `system/service/init/realm.cpp:786-788` and `system/service/init/realm.cpp:809-824` show the forward, `kernel/object/include/ours/object/channel_dispatcher.hpp:20-28` the limit, and `examples/echo/echo_server.cpp:273` serves the directory straight after setup.

### W28. Give a file node a VMO nobody can write

- Rule: When a tree server adds a file node, give it a VMO that nobody holds a writable handle to.
- Why: GetBackingMemory hands the client a duplicate of that VMO, even for a private-clone request, which is only a private copy when the contents never change.
- Where: Convention. Stated at `system/lib/ioservice/include/ioservice/tree.hpp:50-55`; the server's side is `system/lib/ioservice/src/server.cpp:440-443`.

### W29. Act on every outcome if you drive Server yourself

- Rule: If you call `ioservice::Server::dispatch()` yourself instead of using `LoopServer`, do what every outcome says: write, close and wait as it tells you.
- Why: The core touches no channel; it only says what to do, so skipping an outcome leaks a handle or leaves a client waiting.
- Where: Convention. `system/lib/ioservice/include/ioservice/server.hpp:23-27` and `system/lib/ioservice/include/ioservice/server.hpp:74-104` list the outcomes, and `system/lib/ioservice/src/loop_server.cpp:112-171` is the complete example.

## Testing

### T1. Keep decisions apart from system calls

- Rule: Put the server's decisions in code that makes no system calls, so a test can run it on the build machine.
- Why: A host test is fast and can force cases a booted image reaches only with trouble, such as a full table or a failed write.
- Where: Convention. The pattern is `ioservice::Server::dispatch()`, which takes one message and returns what to do (`system/lib/ioservice/README.md:12`), and the Loop's host test links a fake of exactly the three calls the Loop makes (`system/lib/loop/include/loop/loop.hpp:283-285`). [05-testing-and-debugging.md](05-testing-and-debugging.md) shows how.

### T2. Test every refusal

- Rule: Write a test for each way a request can fail: a malformed message, an unknown method, a full table, a reply that cannot be written, and a client that leaves.
- Why: A run that takes only the happy path never reaches these, and each is a place where a handle leaks or a client is left waiting.
- Where: Convention. Models: the decoder's refusals at `examples/echo/protocol/test/ours_examples_echo_test.cpp:79-125`, and the server's at `system/lib/ioservice/test/loop_server_test.cpp:382-421` and `system/lib/ioservice/test/outgoing_test.cpp:239-277`.

### T3. Put host tests where the build finds them

- Rule: Make your test an executable marked `testonly` in the host toolchain and reachable from `//:default`.
- Why: The boot check finds host tests by asking GN, so a test outside the default build is never run.
- Where: Enforced once it is found: a failing test fails the check (`host-tests FAIL`). The rule is `scripts/host_tests.py:3-9`, and `.agents/skills/ours-verify/SKILL.md:25-31` describes the run.

### T4. Boot both images

- Rule: Before you call a change done, run the boot check, which builds and boots both images: `python3 .agents/skills/ours-verify/scripts/boot_check.py`.
- Why: The debug image stops at the first failed assertion and the release image runs on past it, so only the two together show that no assertion is hiding a bug; a clean compile is not a working server.
- Where: Enforced by the script's exit code. `.agents/skills/ours-verify/SKILL.md:6-10` and `.agents/skills/ours-verify/SKILL.md:17-23` describe it, and `.agents/skills/ours-verify/scripts/boot_check.py:66-71` says why there are two images.

### T5. Make the boot prove your route

- Rule: Add an on-target check that connects to your server through the real route, and have it print a line the boot check requires.
- Why: Starting and being reached are different steps; before `routing_checks` reached echo, the routes through `expose` and the forwarding into a provider's outgoing directory were tested only on the host.
- Where: Enforced once the line is listed. The lines are `REQUIRED` in `.agents/skills/ours-verify/scripts/boot_check.py:88-100`, the model is `system/service/init/test/routing_checks/routing_checks.cpp:793-826`, and the history is in `examples/echo/echo_server.cpp:7-12`.

### T6. Test the routes that must fail

- Rule: Check that a use nobody offers answers NotFound, that a narrowed offer answers AccessDenied, and that a route to a server that has exited answers NotFound.
- Why: These are the answers a client gets when its manifest is wrong, and each leaves one line in the root's log naming the target, the hop and the capability, so a failing route can be found from the log alone.
- Where: Enforced by `routing_checks` for the dev image (`system/service/init/test/routing_checks/routing_checks.cpp:525-541`); one Open that races the exit may read PeerClosed once, which the check allows (`system/service/init/test/routing_checks/routing_checks.cpp:805-825`). The root's lines come from `system/service/init/realm.cpp:194-203`:

```text
init: routing_checks's use of "ours.test.Unoffered" routes nowhere: NotFound at . (offer of "ours.test.Unoffered")
```

That is `docs/changes/261004/init.md:438`.

```text
init: pc-ps2's use of "ours.resource.Irq" reaches another protocol: it wants ours.resource.Irq, the route ends at ours.process.Launcher (the built-ins)
```

That is `docs/design/261029/protocol-identity.md:71`. [05-testing-and-debugging.md](05-testing-and-debugging.md) shows how to read them.

## Review

Review adds no rules. Each item is a walk over the diff that turns earlier items into questions.

### R1. Walk every place a callback returns

- Rule: At each return, check that Rearm follows a read of everything that raised the signal, and that Close follows freeing the client's record.
- Why: These are the two silent failures of a callback: a spin that burns the thread, and an entry that shrinks the table.
- Where: Convention. Rests on [W6](#w6-answer-rearm-only-after-reading) and [W9](#w9-free-your-record-of-a-client-when-you-close-it).

### R2. Walk every handle

- Rule: For each handle the code creates or receives, follow it to where it is closed, on every path out, the error paths included.
- Why: A leaked handle keeps its object alive, so a client never sees its peer close; a handle closed by the wrong party leaves the Loop holding a wait on something that is gone.
- Where: Convention. Rests on [D4](#d4-write-down-who-closes-every-handle), [W12](#w12-close-the-handles-of-a-message-you-do-not-use), [W16](#w16-after-a-failed-add-the-handle-is-still-yours) and [W17](#w17-a-channel-handed-to-connect-is-yours-to-serve-or-close).

### R3. Walk every table

- Rule: For each fixed array, find its size, the line that checks it against its limit, and the line that answers a client when it is full.
- Why: A table with no answer for "full" leaves the newcomer waiting, and one with no check against `PORT_MAX_OBSERVERS` meets that limit in a place nobody planned.
- Where: Convention. Rests on [D3](#d3-write-down-every-limit-and-what-it-answers), [W15](#w15-refuse-a-connection-you-cannot-take-with-an-epitaph) and [W18](#w18-size-the-table-of-slots-and-check-it-at-build-time).

### R4. Walk every message path

- Rule: For each kind of message and each way it can fail, find the line that replies, closes the connection or sends an epitaph.
- Why: A path that does none of them leaves a client waiting for an answer that never comes.
- Where: Convention. Rests on [W13](#w13-close-a-client-that-leaves-the-protocol), [W14](#w14-end-the-connection-when-a-reply-cannot-be-written) and [W15](#w15-refuse-a-connection-you-cannot-take-with-an-epitaph).

### R5. Walk every line that spells the protocol's name

- Rule: Find the manifest's `capabilities` and `expose`, the `add_protocol` call, each client's `use` and the root's `offer`, and check they agree on the name, and on the type where an offer renames.
- Why: A disagreement is a valid route that ends nowhere, and the symptom is a NotFound or a PeerClosed far from the typo.
- Where: Convention for the code, Enforced for the types. Rests on [W22](#w22-declare-the-protocol-and-expose-it), [W23](#w23-spell-the-protocols-name-the-same-everywhere) and [W24](#w24-state-the-type-where-a-rename-sits-between).

### R6. Search for what the runtime lacks

- Rule: Search the diff for `new`, for types that allocate, for global objects with constructors and for virtual destructors.
- Why: The first is a link error, but the others fail late or silently: a global that needs a constructor starts as zeros.
- Where: Convention. Rests on [W1](#w1-allocate-nothing), [W2](#w2-make-server-state-static-and-constinit) and [W3](#w3-give-a-base-class-no-virtual-destructor).

### R7. Run it and read the log

- Rule: Run the host tests and boot both images, then read the log for your component's lines: none should say "routes nowhere" or "reaches another protocol" unless a test planned it.
- Why: A passing build says the code compiles; only the boot says the route works, and the root's log is where a wrong route shows.
- Where: Enforced by the boot check for what it requires; the rest is Convention. Rests on [T3](#t3-put-host-tests-where-the-build-finds-them), [T4](#t4-boot-both-images) and [T6](#t6-test-the-routes-that-must-fail).

## Where to look next

- [01-concepts.md](01-concepts.md): the ideas behind each rule, in the order a reader needs them.
- [03-protocols.md](03-protocols.md): how to define the messages (W10 to W15 assume them), including [closed protocols](03-protocols.md#closed-protocols).
- [04-tree-servers.md](04-tree-servers.md): how a tree server answers an Open (W28 and W29).
- [05-testing-and-debugging.md](05-testing-and-debugging.md): how to test on the host, boot, and read a failing route (T1 to T6).
- [debugging.md](../development/debugging.md): for a boot that panics or hangs; it is written in Chinese.
- [The component framework glossary](../architecture/component-framework/glossary.md): the longer list of words.

## Sources

- `system/lib/loop/include/loop/loop.hpp`
- `examples/echo/echo_server.cpp`
- `examples/echo/BUILD.gn`
- `examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp`
- `examples/echo/protocol/test/ours_examples_echo_test.cpp`
- `system/lib/ioservice/README.md`
- `system/lib/ioservice/include/ioservice/{loop_server,outgoing,server,tree}.hpp`
- `system/lib/ioservice/src/{loop_server,server}.cpp`
- `system/lib/ioservice/test/{loop_server,outgoing}_test.cpp`
- `system/lib/wire/include/wire/wire.hpp`
- `system/lib/ort/src/{cpp_dependencies,start}.cpp` and `system/lib/ort/include/ort/debug.hpp`
- `system/public/ours/{syscall,port}.hpp`
- `kernel/object/include/ours/object/channel_dispatcher.hpp`
- `system/component_manifest.py` and `system/test_component_manifest.py`
- `system/service/init/realm.cpp`, `system/service/init/test/routing_checks/{BUILD.gn,routing_checks.cpp}`
- `kernel/products/image/dev/BUILD.gn`
- `docs/design/261004/component-framework.md`
- `docs/design/261029/protocol-identity.md`
- `docs/changes/261004/init.md`
- `scripts/host_tests.py`
- `.agents/skills/ours-verify/SKILL.md` and `.agents/skills/ours-verify/scripts/boot_check.py`

Checked against commit `693260d0` on 2026-10-29.
