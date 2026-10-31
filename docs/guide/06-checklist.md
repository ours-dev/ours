# Checklist: writing and reviewing a server

## What you will be able to do

- Check a server's design, code, tests and review against the rules the code itself lays down, in the order you meet them.
- Say, for each rule, why it exists and where it is stated or enforced, so you can look it up instead of taking it on trust.
- Tell which rules the build, the Loop or the root will catch for you, and which only a reader will catch.

This chapter does not teach. It is the list to keep open while you work, and each item points at the chapter or the source that does the teaching. It has 48 items, and nobody reads them in one sitting:

- The ten rules below are the front page. Read them first.
- "What a first version needs" picks the thirteen items a first boot cannot do without.
- The rest is reference, read by phase: Design before you write, Writing while you write, Testing while you boot, Review with a diff open.

The words are [chapter 1](01-concepts.md)'s: a [handle and a channel](01-concepts.md#handles-and-channels), [signals](01-concepts.md#ports-and-signals), [the Loop and its slots](01-concepts.md#the-loop-and-its-slots), a [manifest](01-concepts.md#components-and-manifests), the [outgoing directory](01-concepts.md#svc-and-the-outgoing-directory) and an [epitaph](01-concepts.md#epitaphs). Four more are used the same way throughout:

- A *server* is a component with a program that answers requests on channels.
- *The root* is the root component (its manifest is `meta/root.cm`, and it has no program of its own); `init` is the program that runs it, and its log lines start with `init:`.
- *The dev image* is `kernel.dev.obi`, the image the guide boots ([README.md](README.md#build-and-boot-the-dev-image)).
- A *callback* is the `on_signals()` function the Loop calls when a slot's wait fires; ["The callback that W4 to W9 bind"](#the-callback-that-w4-to-w9-bind) shows it.

## The ten rules that matter most

1. Allocate nothing: a server has no heap, so every table is a fixed, `constinit` array ([W1](#w1-allocate-nothing), [W2](#w2-make-server-state-static-and-constinit)).
2. A callback does one unit of work and never waits ([W4](#w4-do-one-unit-of-work-per-callback), [W7](#w7-never-wait-inside-a-callback)).
3. A callback closes no handle and arms no wait: it answers Close or Rearm and the Loop does the rest ([W5](#w5-close-no-handle-and-arm-no-wait-in-a-callback)).
4. Answer Rearm only after you have read what raised the signal, or the server spins ([W6](#w6-answer-rearm-only-after-reading)).
5. Wait for Readable and PeerClosed together, and treat a callback without Readable as the client gone ([W8](#w8-wait-for-readable-and-peerclosed-together)).
6. Know who owns each handle at each moment, and close every handle you will not use, including those inside a message you reject ([D4](#d4-write-down-who-closes-every-handle), [W12](#w12-close-the-handles-of-a-message-you-do-not-use)).
7. Never leave a client waiting for an answer that cannot come: reply, close the client's channel, or send an epitaph ([W13](#w13-close-a-client-that-leaves-the-protocol), [W14](#w14-end-the-connection-when-a-reply-cannot-be-written), [W15](#w15-refuse-a-connection-you-cannot-take-with-an-epitaph)).
8. Size every table when you build, and decide what is answered when it is full ([D3](#d3-write-down-every-limit-and-what-it-answers), [W18](#w18-size-the-table-of-slots-and-check-it-at-build-time)).
9. Keep alive, and in place, everything the Loop and the libraries point at ([W19](#w19-keep-alive-everything-the-loop-points-at)).
10. Spell the protocol's name the same in the manifest, the code and the clients, and boot both images to prove the route ([W23](#w23-spell-the-protocols-name-the-same-everywhere), [T4](#t4-boot-both-images), [T5](#t5-make-the-boot-prove-your-route)).

## What a first version needs

A first version is a server that starts, is reached and answers one request. Of the 41 items in Design, Writing and Testing, these thirteen are the ones it cannot do without. The echo server meets each of them, so `examples/echo` is the pattern to copy; the other items make a server hold up under load, failure and review, and you read them as it grows.

| Item | What it protects |
|---|---|
| [D1](#d1-choose-the-kind-of-server-first) | You build the right parts: a protocol server and a tree server are made of different ones. |
| [W1](#w1-allocate-nothing) | The program builds and runs: the runtime has no heap. |
| [W2](#w2-make-server-state-static-and-constinit) | Your tables start as built, not as zeros: nothing runs the constructors of a program's globals. |
| [W5](#w5-close-no-handle-and-arm-no-wait-in-a-callback) | The Loop's slot: only the Loop closes a slot's handle or arms its wait, and the callback answers Close or Rearm. |
| [W6](#w6-answer-rearm-only-after-reading) | The CPU: a Rearm over an unread message calls the callback again at once, for ever. |
| [W8](#w8-wait-for-readable-and-peerclosed-together) | The slot table: a client that leaves is seen as a callback without Readable. |
| [W17](#w17-a-channel-handed-to-connect-is-yours-to-serve-or-close) | The client's channel: `Handler::connect` must serve it or close it, or the client waits for good. |
| [W22](#w22-declare-the-protocol-and-expose-it) | The route: the protocol is declared and exposed, or a client's route ends in NotFound. |
| [W23](#w23-spell-the-protocols-name-the-same-everywhere) | The route: one spelling of the name in the manifest, the code and the client's use. |
| [W25](#w25-put-the-component-in-the-image-and-offer-it) | The component: the root starts it only if the root manifest lists it, and a client reaches it only through an offer. |
| [W26](#w26-build-the-program-under-bin-in-a-user-toolchain) | The program: the root runs only what is installed under `bin/`, built in a user toolchain. |
| [T4](#t4-boot-both-images) | The verdict: a clean compile is not a working server. |
| [T5](#t5-make-the-boot-prove-your-route) | The proof: a client in the booted image reaches the server through the real route. |

## How to read an item

Every item has three parts, one line each:

- Rule: what to do, in one sentence.
- Why: what goes wrong without it.
- Where: the file and line where it is stated or enforced.

The Where line starts with one of two words. *Enforced* means something stops or reports a violation: a compile or build error, a status code at run time, or a failed check in a boot, and the line names it. *Convention* means nothing checks it; only a reader does, so these are the rules to look for hardest in a review. Where a rule is part of each, the line says which part.

The phases are in the order you meet them: design (D), writing (W), testing (T) and review (R). Review does not add rules; it turns the earlier ones into walks over a diff. A snippet is copied exactly from the file and lines named under it.

## Design

Decide these before you write code. They are cheap to decide and costly to change.

### D1. Choose the kind of server first

- Rule: Decide whether you are writing a protocol server (it answers a protocol of its own on each client's channel) or a tree server (it serves a directory tree of files and directories), before anything else.
- Why: The two are built from different parts, and a wrong first choice means a rewrite: a protocol server is your own handler and message codec behind `ioservice::Outgoing`, while a tree server is an `ioservice::Tree` with nodes you add, served by `Server` and `LoopServer`. A manifest cannot declare or expose a directory, so no route reaches a tree server of your own; anything another component must call is a protocol server.
- Where: Convention. `system/lib/ioservice/README.md:7-10` lists the parts; [03-protocols.md](03-protocols.md) and [04-tree-servers.md](04-tree-servers.md) explain when each fits. The manifest limit is Enforced at `system/component_manifest.py:466-470` and `system/component_manifest.py:649-653`.

### D2. Plan for one thread that never waits

- Rule: Break the work into steps that each finish quickly and never wait for anything, because one thread runs every step for every client.
- Why: The Loop has no tasks, no timers and no extra threads, so steps run one at a time to completion; a slow step delays every client, and a step that waits for an answer from its own program waits for ever, because the thread that would read the answer is the one waiting.
- Where: Convention (nothing stops a program from calling something slow). Stated at `system/lib/loop/include/loop/loop.hpp:13-19` and `system/lib/ioservice/include/ioservice/outgoing.hpp:42-43`.

### D3. Write down every limit and what it answers

- Rule: For every table and queue the server has, write down its size and what a client is told when it is full.
- Why: With no heap each table is a fixed array, and once it is full the only choices are to refuse the newcomer or leave it waiting; the kernel has limits of its own, and you meet them as status codes, not as crashes.
- Where: Convention for your own tables; the model is `examples/echo/echo_server.cpp:52-61` and `examples/echo/echo_server.cpp:168`. The limits you do not choose are these:

| Limit | Value | What you meet | Where |
|---|---|---|---|
| Bytes in one channel message | 4096 | `wire` refuses to build a bigger message (`InvArgs`) | `system/public/ours/syscall.hpp:43`, `system/lib/wire/include/wire/wire.hpp:190-194` |
| Handles in one channel message | 64 | the size your request-handle buffer needs | `system/public/ours/syscall.hpp:46` |
| Messages waiting on one end of a channel | 64 | a write answers `NoResource` and the message is consumed | `kernel/object/include/ours/object/channel_dispatcher.hpp:20-28`, `kernel/object/include/ours/object/channel_dispatcher.hpp:57-61` |
| Waits one port holds at once | 1024 | `Loop::add` can answer `NoResource` | `system/public/ours/port.hpp:167`, `system/lib/loop/include/loop/loop.hpp:192-197` |
| Handles one process has created | 1024 | creating one more answers `NoResource`; closing one gives the room back | `system/public/ours/syscall.hpp:59` |
| Protocols in one outgoing directory | 8 | `add_protocol` answers `NoResource` | `system/lib/ioservice/include/ioservice/outgoing.hpp:69`, `system/lib/ioservice/include/ioservice/outgoing.hpp:128` |
| Open connections to the outgoing directory | 4 | an Open past them is refused with the epitaph `NoResource` | `system/lib/ioservice/include/ioservice/outgoing.hpp:71-76`, `system/lib/ioservice/src/server.cpp:217-222`, `system/lib/ioservice/src/server.cpp:529-531` |
| Nodes in one `ioservice::Tree` | the storage you give | an add answers `NoResource` | `system/lib/ioservice/include/ioservice/tree.hpp:88-90` |
| Bytes in one debug write | 256 | a longer line goes out in several writes and can interleave with another program's | `system/public/ours/syscall.hpp:69`, `system/lib/ort/include/ort/debug.hpp:32-39` |

### D4. Write down who closes every handle

- Rule: For every handle the server ever holds, write down who owns it at each moment and who closes it.
- Why: A handle that is never closed keeps its object alive, and a channel end you forgot to close means the client's end never sees PeerClosed; a handle closed by the wrong party leaves the Loop waiting on something that is gone.
- Where: Convention. The hand-over points the libraries state are in this table:

| Handle | Whose | Closed by | Where |
|---|---|---|---|
| The handle you give `Loop::add` | the Loop's if `add` answers Ok, yours if it fails | the Loop, when the slot's callback answers Close | `system/lib/loop/include/loop/loop.hpp:221-226` |
| The port you give `Loop` | the Loop's from then on | the Loop's destructor | `system/lib/loop/include/loop/loop.hpp:199`, `system/lib/loop/include/loop/loop.hpp:207-209` |
| The DirectoryRequest handle you give `Outgoing::serve` | the Loop's on Ok, yours on failure | the Loop, as above | `system/lib/ioservice/include/ioservice/outgoing.hpp:138-141` |
| A client's channel handed to `Handler::connect` | yours from the call | you, or the Loop once you `add` it | `system/lib/ioservice/include/ioservice/outgoing.hpp:52-54` |
| Handles that arrive in a message | yours | you | `examples/echo/echo_server.cpp:223-225` |

### D5. Decide that your protocol is closed

- Rule: Decide that a message with a method you do not know, or one your decoder refuses, closes the client's channel and is not answered.
- Why: A peer that sends such a message is not speaking your protocol, no answer would help it, and the header has no bit that marks a call as one an unknown method may be answered for; the peer learns by seeing the channel close.
- Where: Convention for your own server; `ioservice` does it for the file protocol (`system/lib/ioservice/include/ioservice/server.hpp:83-90`), and `examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:39-40` states it for Echo. [03-protocols.md](03-protocols.md#closed-protocols) explains the rule.

### D6. Decide how the server ends

- Rule: Decide how your server stops (never, when a client asks, or on an error it cannot recover from), and expect that nothing will start it again.
- Why: The root marks a component that has exited and never starts it again; a later route to it answers NotFound, and channels already open to it see PeerClosed.
- Where: Enforced by the root: `system/service/init/realm.cpp:790-794` answers NotFound for an exited provider, and `system/service/init/realm.cpp:861-866` marks the exit. How a program stops is Convention (W20).

## Writing

The items run in this order: storage (W1 to W3), the callback (W4 to W9), messages (W10 to W15), ownership (W16, W17), limits and lifetimes (W18, W19), leaving (W20, W21), manifest and routing (W22 to W27) and, for a tree server, W28 and W29. The snippets are copied from `examples/echo`, the protocol server that runs in the dev image.

### W1. Allocate nothing

- Rule: Do not use `new`, `malloc` or any type that allocates, such as `std::vector`.
- Why: A user program has no heap: the runtime (`ort`, see [words used elsewhere](01-concepts.md#words-used-elsewhere)) defines no `operator new` at all, and its `operator delete` traps on any pointer that is not null.
- Where: Enforced at link time and at run time: `system/lib/ort/src/cpp_dependencies.cpp:27-35` says an accidental `new` stays a link error that names the line, and `system/lib/ort/src/cpp_dependencies.cpp:16-24` traps a delete of a real pointer.

### W2. Make server state static and constinit

- Rule: Keep the server's tables in namespace-scope `constinit` variables, and put types with destructors in `ustl::LazyInit`, which `main()` builds in place.
- Why: Nothing runs the constructors of a program's global objects, so a global that needs one would silently start out as zeros; `constinit` makes the compiler reject such a global, and keeping the tables off the stack matters because the echo server's state is about 8 KiB.
- Where: Enforced by the compiler for `constinit`; that you use it is Convention. The reason is given at `system/lib/ort/src/start.cpp:42-45`. The echo server's three declarations are at `examples/echo/echo_server.cpp:279-281` and its reason for `LazyInit` at `examples/echo/echo_server.cpp:273-278`; [01-concepts.md](01-concepts.md#where-a-servers-memory-comes-from) shows them and explains `ustl::LazyInit`. The size note is at `examples/echo/echo_server.cpp:126-127`.

### W3. Give a base class no virtual destructor

- Rule: This rule is unusual, because nothing is ever deleted here: give a class you derive handlers from a protected, non-virtual destructor, as `ioservice::Handler` does.
- Why: Nothing is ever deleted, since there is no heap, so a virtual destructor only adds a deleting destructor that calls `operator delete`, which traps on a real pointer.
- Where: Convention. `system/lib/ioservice/include/ioservice/outgoing.hpp:46-49` and `system/lib/ioservice/include/ioservice/outgoing.hpp:57-61` do it for `Handler`, and `system/lib/loop/include/loop/loop.hpp:101-105` for `loop::Waiter`; `system/lib/ort/src/cpp_dependencies.cpp:27-35` says why `operator delete` exists at all.

### The callback that W4 to W9 bind

W4 to W9 are rules for one function, `on_signals()`, which this chapter calls the callback, and W9 is also a rule for a second one, `on_rearm_failed()`. A slot of the Loop holds a pointer to a `loop::Waiter`, an object of your own class, and when the slot's wait fires the Loop calls its `on_signals()` with the signals the wait observed (`system/lib/loop/include/loop/loop.hpp:106-117`).

Its declaration is in [chapter 1](01-concepts.md#the-loop-and-its-slots): two virtual functions, `on_signals(ours::Signals observed) -> loop::Action`, and `on_rearm_failed(ours::Status cause) -> void`, whose default does nothing.

The answer of `on_signals()` is `loop::Action::Rearm` (wait again for the same signals) or `loop::Action::Close` (the Loop closes the slot's handle and frees the slot). `on_rearm_failed()` answers nothing: the Loop calls it once, after an answer of Rearm, when it could not arm the wait again, and it has closed the handle and freed the slot by then. In the echo server each client is a `Waiter` whose `on_signals()` hands the call to the server's `serve()` (`examples/echo/echo_server.cpp:113-115`) and whose `on_rearm_failed()` hands it to `rearm_failed()` (`examples/echo/echo_server.cpp:120-122`). [02-first-server.md](02-first-server.md) builds one from an empty program.

### W4. Do one unit of work per callback

- Rule: In one callback do one unit of work, such as reading one message or reporting one exit, and answer.
- Why: A client with more messages queued is not called again at once; its next wait queues behind everyone else's, so a busy client is served in turns instead of starving the rest.
- Where: Convention for the callback; the Loop's side is Enforced. The rule is stated at `system/lib/loop/include/loop/loop.hpp:50-53`, and the Loop's side at `system/lib/loop/include/loop/loop.hpp:295-298`.

### W5. Close no handle and arm no wait in a callback

- Rule: A callback never closes its own handle (it answers Close), never closes another slot's handle, and never arms a wait (it answers Rearm, and the Loop arms; if the Loop cannot, it tells the waiter, see W9); it may `add()` new slots.
- Why: The Loop closes a slot's handle only after the callback returns, when the slot's one wait has fired and its packet is taken, so nothing of the slot is left on the port; the Loop cannot withdraw a packet that is already queued, and it keeps at most one live wait per slot.
- Where: Convention (nothing stops a callback from calling `handle_close`). Stated at `system/lib/loop/include/loop/loop.hpp:50-53` and `system/lib/loop/include/loop/loop.hpp:232-233`, with the reasons at `system/lib/loop/include/loop/loop.hpp:289-294` and `system/lib/loop/include/loop/loop.hpp:299-310`.

### W6. Answer Rearm only after reading

- Rule: Answer Rearm only once the callback has consumed what raised the signal.
- Why: The wait is level-triggered, so it fires again as long as the signal is still raised; a callback that rearms with its message unread, or with only PeerClosed raised (which stays raised), is called again on every turn, and the server spins on the one thread's CPU.
- Where: Convention (nothing detects a spin). Stated at `system/lib/loop/include/loop/loop.hpp:55-62`.

### W7. Never wait inside a callback

- Rule: Never wait inside a callback or inside a function it calls: write replies and move on.
- Why: A callback that waits for a reply from its own program blocks the very thread that must read it, and one that waits for anyone else holds every other client back; a write to a full channel fails at once, so nothing needs to wait for room.
- Where: Convention. `system/lib/ioservice/include/ioservice/outgoing.hpp:52-54` and `system/lib/ioservice/include/ioservice/loop_server.hpp:46-49` say a function called from a callback "must not wait"; the write that does not wait is at `kernel/object/include/ours/object/channel_dispatcher.hpp:57-61`.

### W8. Wait for Readable and PeerClosed together

- Rule: Wait for Readable and PeerClosed together, read whenever Readable is raised, and treat a callback without Readable as the client gone.
- Why: A client that leaves with nothing left to read raises only PeerClosed, so a server that asked only for Readable never learns it left and its slot, handle and table entry stay for good; but a closed channel still delivers its last messages first, so a callback with both signals must still read.
- Where: Convention. `examples/echo/echo_server.cpp:159-160` asks for both and `examples/echo/echo_server.cpp:189-191` takes the missing Readable as the end; `system/lib/ioservice/src/loop_server.cpp:15` and `system/lib/ioservice/src/loop_server.cpp:101-104` do the same, and the channel's rules are at `kernel/object/include/ours/object/channel_dispatcher.hpp:40` and `kernel/object/include/ours/object/channel_dispatcher.hpp:67-69`.

```cpp
        if (!(observed & Signals::Readable)) {
            return release(client);
        }
```

That is `examples/echo/echo_server.cpp:189-191`.

### W9. Free your record of a client when you close it

- Rule: When a callback answers Close, free your own record of that client in the same callback; when the Loop tells you it could not arm your wait again (`on_rearm_failed()`), free it there.
- Why: The Loop frees the slot and closes the handle after the callback returns, but your table entry is yours to free, and the callback is the one place that knows the client is gone; an entry never freed makes the table one client smaller for good. The Loop frees a slot in a second case, when it could not arm the wait again after an answer of Rearm (the kernel can refuse, `NoMemory` for one). The callback has returned by then and nothing else knows the entry, so `on_rearm_failed()` is the only place left to free it, and it is called once, after the slot is free: it must not touch the handle, which is closed. A record not freed there is lost for good, as one not freed after Close.
- Where: Convention. The echo server's one exit for a finished client:

```cpp
    /// Frees |client|'s entry; its slot closes the channel, by answering
    /// Close.
    static auto release(Client &client) -> loop::Action {
        client.channel = Uandle::Invalid;
        return loop::Action::Close;
    }
```

That is `examples/echo/echo_server.cpp:171-176`; `examples/echo/echo_server.cpp:150-153` is the search for a free entry that depends on it, and `examples/echo/echo_server.cpp:90-102` says why an entry is safe to take again. The same entry freed after a failed re-arm (the Loop has closed the channel, so only the entry is left, and the cause is logged):

`examples/echo/echo_server.cpp:178-184`:

```cpp
    /// Frees |client|'s entry after the Loop failed to arm its wait again.
    /// The Loop has closed the channel and freed the slot, so the entry is
    /// all that is left to free.
    auto rearm_failed(Client &client, Status const cause) -> void {
        client.channel = Uandle::Invalid;
        report("cannot wait on a client again", cause);
    }
```

### W10. Read one message into buffers that hold the largest

- Rule: Read exactly one message per callback, into request buffers that hold the largest message a channel carries, in bytes and in handles.
- Why: A message that does not fit stays at the head of the queue and the read answers BufferTooSmall, so the same message fails every time and the client can never be served again.
- Where: Convention; the echo server sizes its arrays with the wire constants. Stated at `system/lib/ioservice/include/ioservice/loop_server.hpp:121-126`, `examples/echo/echo_server.cpp:133-134` and `kernel/object/include/ours/object/channel_dispatcher.hpp:69-72`.

### W11. Finish with a decoded request before the callback returns

- Rule: Use a decoded request before the callback returns, and build the reply in a different buffer from the one you decoded from.
- Why: A decoded string or byte span is a view into the request buffer, not a copy; one buffer serves every callback, so the next message overwrites it, and a reply built in the same buffer would overwrite the bytes it is copying from.
- Where: Convention. `examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:69-73` says the decoded value views the message's bytes, and `examples/echo/echo_server.cpp:232-236` keeps the two buffers apart.

### W12. Close the handles of a message you do not use

- Rule: Close every handle a message carried that you do not use, including those in a message you reject.
- Why: A handle that arrives in a message is in your handle table; if you never close it the object it names stays alive, and the sender's other end never sees PeerClosed.
- Where: Convention (the `ioservice` code does it for you at `system/lib/ioservice/src/loop_server.cpp:148-151`). The echo server:

```cpp
        for (auto const handle: message.handles) {
            auto _ = ours::handle_close(handle);
        }
        return release(client);
```

That is `examples/echo/echo_server.cpp:223-226`.

### W13. Close a client that leaves the protocol

- Rule: When a message has a method you do not know, or your decoder refuses it, close the client's channel and send nothing.
- Why: The peer is not speaking your protocol, so no answer would help it; it learns from the close.
- Where: Convention for your own server. [03-protocols.md](03-protocols.md#closed-protocols) shows the echo server's branch and the rule; the code is `examples/echo/echo_server.cpp:207-226`, where an unknown or refused message falls to the lines of W12 that close its handles and release the client. The protocol states it at `examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:39-40`, and the file protocol's server does the same (`system/lib/ioservice/src/server.cpp:561-565`).

### W14. End the connection when a reply cannot be written

- Rule: When a reply cannot be written, end that client: answer Close and free its record.
- Why: A write fails when the client is gone (PeerClosed) or its queue is full (NoResource), and nothing waits for room, so a reply that was not written will never be written and the client would wait for it for good.
- Where: Convention. The echo server, and `system/lib/ioservice/src/loop_server.cpp:129-139` for the file protocol:

```cpp
        if (auto const written = write(client.channel, reply.unwrap()); written != Status::Ok) {
            return release(client);
        }
```

That is `examples/echo/echo_server.cpp:243-245`. The one reply that may be dropped without ending anything is the last one before the program exits (`examples/echo/echo_server.cpp:249-251`).

### W15. Refuse a connection you cannot take with an epitaph

- Rule: When you cannot take a new client, write an epitaph with the reason on its channel, then close the channel.
- Why: The client has already sent its Open and expects an answer on the channel; the epitaph tells it why it was refused (NoResource, say) instead of leaving it to guess from a bare PeerClosed.
- Where: Convention. The echo server uses it for a full client table (`examples/echo/echo_server.cpp:168`) and for a failed `add` (`examples/echo/echo_server.cpp:164`); its `close_with_epitaph` (`examples/echo/echo_server.cpp:77-84`) is shown in [01-concepts.md](01-concepts.md#epitaphs), and the epitaph's form is `system/lib/wire/include/wire/wire.hpp:35-36`.

### W16. After a failed add the handle is still yours

- Rule: When `Loop::add` fails, the handle is still yours: send an epitaph if the handle is a client's, and close it.
- Why: On success the handle belongs to the Loop, but on failure nothing changed, so if you do nothing the handle leaks and its client waits.
- Where: Convention (`add` is `[[nodiscard]]`, so ignoring its result draws a warning). Stated at `system/lib/loop/include/loop/loop.hpp:221-226`; the echo server does it at `examples/echo/echo_server.cpp:159-165`.

### W17. A channel handed to connect is yours to serve or close

- Rule: A channel handed to `Handler::connect` is yours from the call: serve it, usually by adding it to the Loop, or close it.
- Why: The library has given the channel up and nobody else will close it, so a handler that returns without doing either leaks it and its client waits.
- Where: Convention. Stated in the declaration of `Handler::connect`, and, for the lower-level hook, at `system/lib/ioservice/include/ioservice/loop_server.hpp:56-59`:

```cpp
    /// |channel| is the request's, and the function owns it from the call on:
    /// it serves it, typically by adding it to the Loop, or closes it. It
    /// runs inside a Loop callback, so it may add() slots but must not wait.
    virtual auto connect(ours::Uandle channel) -> void = 0;
```

That is `system/lib/ioservice/include/ioservice/outgoing.hpp:52-55`.

### W18. Size the table of slots and check it at build time

- Rule: Give the Loop one slot for every handle that can wait at once, never more than `PORT_MAX_OBSERVERS`, and say so with a `static_assert`.
- Why: Each slot in use holds one live wait on the port and the port allows 1024; a larger table could meet NoResource from the port in a place where you planned no answer.
- Where: Enforced only by the `static_assert` you write; without one, `add()` answers NoResource at run time (`system/lib/loop/include/loop/loop.hpp:228`). Stated at `system/lib/loop/include/loop/loop.hpp:192-197`. The echo server counts one slot per connection to its outgoing directory (`ioservice::Outgoing::MAX_CONNECTIONS`) and one per client (`MAX_CLIENTS`, 4; the dev image's routing checks use one), as [01-concepts.md](01-concepts.md#the-loop-and-its-slots) shows, and checks the sum:

```cpp
// Each slot in use holds one live wait on the port (<loop/loop.hpp>).
static_assert(SLOTS <= ours::PORT_MAX_OBSERVERS);
```

That is `examples/echo/echo_server.cpp:60-61`.

### W19. Keep alive everything the Loop points at

- Rule: Keep alive, and in place, everything the Loop and the libraries hold a pointer or a view to, for as long as they may use it.
- Why: None of them copies what it is given, because copying needs a heap, so a pointer into a stack frame that has returned, or into an object that has moved, is read later by the Loop.
- Where: Convention, except that copying and moving the library objects is Enforced: they have deleted copy constructors. What must outlive what:
  - the table of slots outlives the Loop (`system/lib/loop/include/loop/loop.hpp:190-191`);
  - the `Waiter` a slot calls stays where it is from `add()` until the slot is freed (`system/lib/loop/include/loop/loop.hpp:91-99`), and, after a re-arm that failed, until its `on_rearm_failed()` returns;
  - the `Outgoing` storage and the Loop outlive the `Outgoing`, and the protocol name (kept as a view) and the handler (kept by address) outlive the directory (`system/lib/ioservice/include/ioservice/outgoing.hpp:113`, `system/lib/ioservice/include/ioservice/outgoing.hpp:122-124`);
  - a directory tree's node names, and a file node's VMO ([words used elsewhere](01-concepts.md#words-used-elsewhere)) and bytes, outlive the `Tree` (`system/lib/ioservice/include/ioservice/tree.hpp:47-48`, `system/lib/ioservice/include/ioservice/tree.hpp:115-116`).

The deleted copies are at `system/lib/loop/include/loop/loop.hpp:211-214`, `system/lib/ioservice/include/ioservice/outgoing.hpp:116-118` and `system/lib/ioservice/include/ioservice/tree.hpp:101-103`. In the echo server the long-lived objects are statics (`examples/echo/echo_server.cpp:279-281`) or locals of `main` that never return, because `run()` does not (`examples/echo/echo_server.cpp:299-301`, `examples/echo/echo_server.cpp:314-316`), and its clients lie in storage that never moves (`examples/echo/echo_server.cpp:90-102`).

### W20. Leave by exiting, and say why

- Rule: To stop, end the process from inside the callback with `ours::process_exit`, answering the client first; when you stop because something failed, log why and return a non-zero code from `main`.
- Why: The Loop has no quit and `run()` returns only when the port itself fails, so a server that should stop must end its process; everything the process holds closes as it dies, and the root logs the exit code, which is how a failed start is found.
- Where: Convention for what you choose; Enforced that the root logs an exit, for echo `init: echo exited with 0` (`system/service/init/realm.cpp:858-859`). `system/lib/loop/include/loop/loop.hpp:256` says `run()` never returns Ok. The echo server's Stop:

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

That is `examples/echo/echo_server.cpp:258-267`, explained at `examples/echo/echo_server.cpp:249-257`. After the exit, later routes to the server answer NotFound (`system/service/init/realm.cpp:790-794`) and channels already open to it see PeerClosed.

### W21. Keep each log line short

- Rule: Log every failure you cannot answer, in one line of at most 256 bytes that starts with the program's name.
- Why: The console is the only place a failed server speaks; a line longer than one debug write goes out in several and can interleave with another program's, and the name says whose line it is.
- Where: Convention. `examples/echo/echo_server.cpp:63-66` is the pattern, and `system/lib/ort/include/ort/debug.hpp:32-39` and `system/public/ours/syscall.hpp:69` give the limit.

[Chapter 2](02-first-server.md#level-2-a-client-can-reach-it) does W22 to W26 for a real server, and boots it.

### W22. Declare the protocol and expose it

- Rule: List the protocol under `capabilities` and `expose` it from `self`, or no client can be routed to it.
- Why: The root finds a provider by walking from the client's use up through offers and down through exposes to a declared capability; a missing declaration or expose ends the walk in NotFound.
- Where: Enforced: the build refuses an expose of an undeclared capability (`system/component_manifest.py:664-671`), and a route that ends early is logged by the root (`system/service/init/realm.cpp:195-203`). The echo server's manifest is `examples/echo/BUILD.gn:33-49`, and [01-concepts.md](01-concepts.md#components-and-manifests) shows it with the offer and the use that go with it.

### W23. Spell the protocol's name the same everywhere

- Rule: Spell the protocol's name the same in the manifest's `capabilities`, in the code that passes it to `add_protocol`, and in every client's `use`, using only the characters a name allows.
- Why: The root forwards an Open under the capability's declared name, and the program's outgoing directory has an entry only for the name it added; if they differ, a good route ends at "no such entry" and the client reads the epitaph NotFound.
- Where: Enforced for the characters and the length, at build time: a letter, digit or underscore, then those or `-` or `.`, at most 100 bytes (`system/component_manifest.py:227-238`). Convention for the match: the code keeps its name in one constant (`examples/echo/protocol/include/ours_examples_echo/ours_examples_echo.hpp:42-46`) but a GN file cannot read it, so the manifest repeats it (`examples/echo/BUILD.gn:39`). The symptom is pinned by the host test at `system/lib/ioservice/test/outgoing_test.cpp:204-216`.

### W24. State the type where a rename sits between

- Rule: When an `offer` renames the protocol with `as`, state the protocol's `type` on the client's `use`, naming the protocol the provider declared.
- Why: A rename changes the name and not the protocol, so the name alone no longer says which protocol a route ends at; without the type, a client could be routed to a different protocol under the same name, and would learn it only as a bare PeerClosed when the server closes on an unknown method.
- Where: Enforced three times, in this order: the build, which stops with an error naming the use, where the route ends and the two ways to fix it (`system/component_manifest.py:1214-1233`); the root on the first Open, which logs a line that says `reaches another protocol` (`system/service/init/realm.cpp:188-198`) and answers the epitaph BadType (`system/service/init/realm.cpp:751-755`); and, last, the closed protocol itself. A `type` goes with one protocol, not a list (`system/component_manifest.py:343-349`), and an `offer` or `expose` takes none (`system/component_manifest.py:563-568`, `system/component_manifest.py:655-660`). [01-concepts.md](01-concepts.md#components-and-manifests) shows echo's renaming offer and the use that states the type.

### W25. Put the component in the image and offer it

- Rule: Add the component as a child of the image's root manifest with `startup = "eager"` written out, and have the root manifest offer the protocol to each component that uses it.
- Why: A component exists only if the root manifest names it, and a client reaches it only through an offer; a use that nobody offers answers NotFound at the root.
- Where: Enforced: `startup` must be written out as eager (`system/component_manifest.py:450-452`), and an unoffered use answers NotFound, which the on-target test `routing_checks` checks (`system/service/init/test/routing_checks/routing_checks.cpp:525-541`). [02-first-server.md](02-first-server.md) shows the edit to the root manifest; the dev image's children are `kernel/products/image/dev/BUILD.gn:88-109`.

### W26. Build the program under bin in a user toolchain

- Rule: Build the program with `user_executable()`, which installs it under `bin/`, and make its `BUILD.gn` fail outside a user toolchain, as the examples do.
- Why: The root makes only the files under `bin/` executable when it reads the boot image, and a user program is built in a user toolchain, which the example's `BUILD.gn` asserts.
- Where: Enforced: the manifest compiler refuses a program installed anywhere else (`system/component_manifest.py:411-415`), and `examples/echo/BUILD.gn:13-15` asserts the toolchain. [02-first-server.md](02-first-server.md) shows the program target and its manifest.

### W27. Serve the outgoing directory first

- Rule: Serve the outgoing directory before you do anything slow, and keep serving it for as long as you run.
- Why: The root forwards each client's Open into your outgoing directory and does not wait; Opens queue in the channel, which holds 64 messages, and past that a forward fails and the client sees PeerClosed with no reason.
- Where: Convention. `system/service/init/realm.cpp:797-799` and `system/service/init/realm.cpp:820-824` show the forward, `kernel/object/include/ours/object/channel_dispatcher.hpp:20-28` the limit, and `examples/echo/echo_server.cpp:308-312` serves the directory straight after setup.

### W28. Give a file node a VMO nobody can write

- Rule: When a tree server adds a file node, give it a VMO that nobody holds a writable handle to.
- Why: GetBackingMemory hands the client a duplicate of that VMO, even for a private-clone request, which is only a private copy when the contents never change.
- Where: Convention. Stated at `system/lib/ioservice/include/ioservice/tree.hpp:50-55`; the server's side is `system/lib/ioservice/src/server.cpp:440-443`.

### W29. Act on every outcome if you drive Server yourself

- Rule: If you call `ioservice::Server::dispatch()` yourself instead of using `LoopServer`, do what every outcome says: write, close and wait as it tells you.
- Why: The core touches no channel; it only says what to do, so skipping an outcome leaks a handle or leaves a client waiting.
- Where: Convention. `system/lib/ioservice/include/ioservice/server.hpp:23-27` and `system/lib/ioservice/include/ioservice/server.hpp:74-104` list the outcomes, and `system/lib/ioservice/src/loop_server.cpp:126-185` is the complete example.

## Testing

### T1. Keep decisions apart from system calls

- Rule: Put the server's decisions in code that makes no system calls, so a test can run it on the build machine.
- Why: A host test is fast and can force cases a booted image reaches only with trouble, such as a full table or a failed write.
- Where: Convention. The pattern is `ioservice::Server::dispatch()`, which takes one message and returns what to do (`system/lib/ioservice/README.md:12`), and the Loop's host test links a fake of exactly the three calls the Loop makes (`system/lib/loop/include/loop/loop.hpp:285-288`). [05-testing-and-debugging.md](05-testing-and-debugging.md) shows how.

### T2. Test every refusal

- Rule: Write a test for each way a request can fail: a malformed message, an unknown method, a full table, a reply that cannot be written, and a client that leaves.
- Why: A run that takes only the happy path never reaches these, and each is a place where a handle leaks or a client is left waiting.
- Where: Convention. Models: the decoder's refusals at `examples/echo/protocol/test/ours_examples_echo_test.cpp:79-125`, and the server's at `system/lib/ioservice/test/loop_server_test.cpp:383-422` and `system/lib/ioservice/test/outgoing_test.cpp:239-277`.

### T3. Put host tests where the build finds them

- Rule: Make your test an executable marked `testonly` in the host toolchain and reachable from `//:default`.
- Why: The verify script ([T4](#t4-boot-both-images)) finds host tests by asking GN, so a test outside the default build is never run.
- Where: Enforced once it is found: a failing test makes the script's `host-tests` line read FAIL (`.agents/skills/ours-verify/scripts/boot_check.py:275-284`). The rule is `scripts/host_tests.py:3-9`; [05-testing-and-debugging.md](05-testing-and-debugging.md#how-the-host-tests-are-found-built-and-run) shows how to wire a test in.

### T4. Boot both images

- Rule: Before you call a change done, run the verify script, which builds and boots both images: `python3 .agents/skills/ours-verify/scripts/boot_check.py`.
- Why: The debug image (`kernel.dev.dbg_1.obi`, `dbg` to the script) stops at the first failed assertion and the dev image (`kernel.dev.obi`, `rel`) runs on past it, so only the two together show that no assertion is hiding a bug; a clean compile is not a working server.
- Where: Enforced by the script's exit code; it says why there are two images at `.agents/skills/ours-verify/scripts/boot_check.py:66-73`. [README.md](README.md#build-and-boot-the-dev-image) describes the run and its output.

### T5. Make the boot prove your route

[Chapter 2, level 5](02-first-server.md#level-5-gate-it) does this for `hello`.

- Rule: Add an on-target check that connects to your server through the real route, and have it print a line the verify script requires.
- Why: Starting and being reached are different steps: a server that starts can still be unreachable because a route is wrong, and only a client in the booted image shows it.
- Where: Enforced once the line is listed. The lines are `REQUIRED` in `.agents/skills/ours-verify/scripts/boot_check.py:90-100`, and the model is `routing_checks`' check of echo, `system/service/init/test/routing_checks/routing_checks.cpp:793-826`. [05-testing-and-debugging.md](05-testing-and-debugging.md#read-the-boot-log) says how to add a line.

### T6. Test the routes that must fail

- Rule: Check that a use nobody offers answers NotFound, and that a route to a server that has exited answers NotFound; for a directory use, check that an offer which gives less than the use asks for answers AccessDenied.
- Why: These are the answers a client gets when its manifest is wrong, and each leaves one line in the root's log naming the client's use, the hop where the walk stopped and the capability, so a failing route can be found from the log alone.
- Where: Enforced by `routing_checks` for the dev image (`system/service/init/test/routing_checks/routing_checks.cpp:525-541`, and `system/service/init/test/routing_checks/routing_checks.cpp:317-319` for the directory); one Open that races the exit may read PeerClosed once, which the check allows (`system/service/init/test/routing_checks/routing_checks.cpp:805-825`). The root's lines are built at `system/service/init/realm.cpp:195-203`, and [05-testing-and-debugging.md](05-testing-and-debugging.md#how-routing_checks-makes-a-negative-case) shows them and how to read them.

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

- Rule: For each kind of message and each way it can fail, find the line that replies, closes the client's channel or sends an epitaph.
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
- Where: Enforced by the verify script for what it requires; the rest is Convention. Rests on [T3](#t3-put-host-tests-where-the-build-finds-them), [T4](#t4-boot-both-images) and [T6](#t6-test-the-routes-that-must-fail).

## Where to look next

- [01-concepts.md](01-concepts.md): the ideas behind each rule, in the order a reader needs them.
- [02-first-server.md](02-first-server.md): a first server built step by step, including the program target, its manifest and the edit to the root manifest that W25 and W26 only name.
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
- `system/component_manifest.py`
- `system/service/init/realm.cpp`, `system/service/init/test/routing_checks/routing_checks.cpp`
- `kernel/products/image/dev/BUILD.gn`
- `scripts/host_tests.py`
- `.agents/skills/ours-verify/scripts/boot_check.py`

Checked against commit `561571b7` on 2026-10-30.
