# ioservice changes

## Title: A Handler node asks a NodeHandler for content, now or later, and Server::complete ends a request that waited (2026-11-03)

**Files**
- `system/lib/ioservice/include/ioservice/tree.hpp`, `src/tree.cpp` — `NodeKind::Handler`; `Tree::add_handler()`, `Tree::handler()`; `resolve()` stops at a Handler node as at a Subtree node or a remote entry (one private `ends_walk()` says which kinds); the node's private value `m_value` is shared by Service, Subtree and Handler nodes
- `system/lib/ioservice/include/ioservice/server.hpp`, `src/server.cpp` — the vocabulary (`ObjectKind`, `Opened`, `Entries`, `OpenResult`, `ReadResult`, `EntriesResult`, `VmoResult`, `Ticket`, `Done`, `Pending`, `Answer`), the abstract class `NodeHandler`, `Action::Pending`, `Server::complete()` (four overloads), `Server`'s optional fourth constructor argument, `Server::release()` answering the held server end; the `Connection` gains `m_role`, `m_cookie`, `m_serial`, `m_waiting` and `m_request`; check 9 is split into `judge_directory()` and `judge_file()`, which the tree's nodes and the kind a handler reports share
- `system/lib/ioservice/src/loop_server.cpp` — one `case Action::Pending` in the switch of `handle()`, because the switch must name every action; it is unreachable until step 9 (below)
- `system/lib/ioservice/test/server_test.cpp`, `tree_test.cpp` — 63 tests more in `ioservice` (75 to 138): 11 in `tree_test.cpp` and 52 in `server_test.cpp`, of which 14 run twice (a handler that answers at once, and one that answers Pending); the existing fixture's table is a `std::vector` sized by a constructor argument (default 4) and its node array 24, so that the handler tests have room; no existing test changed
- `system/lib/ioservice/README.md` — the node kind, the handler, tickets and `complete()`, the storage size
- `docs/repairs/261103/ioservice.md` — two items noticed on the way (the size of a `Connection`; the copy a Read makes)

**Symptom**
Latent, cannot be triggered before step 9: nothing adds a Handler node yet, and no `LoopServer` gives its `Server` a handler. The change is step 8 of 9 of `.agents/tasks/directory-capability/DESIGN.md` (decision 7, option C′, the core's half). A Subtree node hands the program the whole channel, and with it every check the core makes. A filesystem that has to wait on a disk needs the opposite split: the core keeps the protocol and asks only for content, and the answer may come later, which `Server::dispatch()` could not express (it returns an `Outcome` for the message it was given, now).

**Cause**
`Server` answered every request inside `dispatch()`: it read the file's bytes, listed the node's children and duplicated its VMO from the tree it was given. A filesystem has neither a tree of nodes nor the bytes at hand. The design's answer is a node kind whose content a handler answers, with each operation returning `Done(result)` or `Pending(ticket)`. The decisions inside it, which the design left to this step (or says in one place and not another), in the order a reader meets them:

- **Every call is told the node's value.** The design says the node "holds a value that tells the handler which of its trees this is" and the table of operations lists no value. A `Server` has one handler (its optional constructor argument, as `LoopServer` has one `Delegate`), and a program with several Handler nodes needs the call to say which. The value is not kept in the connection: the connection's node is the Handler node, and `value_of(node)` reads it from the tree, so a cookie never has to carry it. `Outgoing` (step 9) can be the handler and look its per-node handler up by value, as it looks a service up for `Delegate::connect()`.
- **An Open through a directory the handler opened is told that directory's cookie.** The design's table gives `open` "the rest of the path, the flags" and says elsewhere that the core passes the cookie back "on every call". A path in an Open on a handler's directory connection is relative to that directory, which only its cookie names, so the call has a second argument, `directory`: none when the Open walked to the Handler node (the path is what is left after the node, `.` for the node itself), the directory's cookie when it came on a connection the handler made (the path is the whole path the client sent). The core does no walk there; a name that is not there is the handler's to refuse. The checks 1 to 7 are the connection's and run first on both.
- **A result is a `ustl::Result` of the operation's payload and a status.** The design's "a result is a status plus the operation's payload" is what `Result<T, Status>` already is, and an `Ok` can never be carried without its payload. `Done` and `Pending` are the design's names: `Done<R>{R result}` and `Pending{Ticket}`, and `Answer<R>` is a `std::variant` of the two (`ustl::Variant` is the same type). The core reads an answer with `get_if`, never `visit` or `get`: a visit that finds the variant valueless throws `bad_variant_access`, and the build has no exceptions and no RTTI, so the test binary does not even link one (the first build of the tests showed it). An `Open`'s payload is `Opened{kind, cookie}`, the kind a two-valued `ObjectKind`.
- **The bytes of a Read or a ReadDirents are the handler's, valid until the core call that received them returns, and the core copies them once.** The cheapest honest rule that serves both answers. Lending the handler the reply buffer would save the copy for a synchronous handler, but a handler that answers Pending cannot write into it: one `ReplyBuffer` serves every callback, and by the time the handler completes it holds another connection's reply. A file of the tree makes the same one copy (`VmoFile::bytes` into the reply), so a handler whose bytes are in memory costs the same; a handler that builds them costs a second, which is item 2 of `docs/repairs/261103/ioservice.md`. A handler that returns more than it was asked for is the handler's bug and is answered `InternalError`; the offset does not move.
- **ReadDirents: the `.` entry is the core's, the cursor is the handler's.** The core sets aside the room for `.` (11 bytes) before it asks, so the handler is given what is left and never has to know; a first call with less room than `.` needs is `BufferTooSmall` without asking, as for a tree. The handler returns whole dirents packed with `prot::io::DirentWriter`, and the cursor to go on from (0 at the start; the core keeps it and never reads it); no dirents at all is the end. A first entry that does not fit is `BufferTooSmall`, and behind the `.` of a first call it is not an error: the reply is the `.` alone, the cursor stays, and the entry is sent first next time, as the tree's listing does. The completion recomputes all of this from the connection's phase, which cannot have changed while the request waited.
- **A ticket is the connection's id and a serial the connection bumps for every request it asks the handler about, the ones answered `Done` included.** One counter is the whole check: the request a ticket names is the latest one and only while it is pending, so `complete()` needs `serial == m_serial` and `m_waiting != None`, and an earlier ticket fails the first even when the connection waits for something else. Bumping only on `Pending` would also work for the double complete, and not for a ticket of a request that was answered at once: that ticket's connection could be pending on the next request with the same serial. The serial starts at 0 with each connection, and the generation of the id keeps one connection's tickets from another's.
- **The pending state keeps more than the design lists.** The design names a flag, the serial and the held server end. `complete()` also needs the request's txid (the reply carries it), its argument (an Open's flags, to judge the kind the handler reports; a Read's count, to refuse more bytes than were asked for; a ReadDirents' byte limit) and the Handler node an Open walked to (the connection the Open came on is the directory it was made on, which says nothing of the handler). That is what grew a `Connection` from 40 to 80 bytes (repair item 1), not the four fields the design counted.
- **`complete()` returns `Result<Outcome, Status>` and refuses before it touches anything.** NotFound for a ticket that names no pending request (its connection released or reused, nothing pending, or a later serial), InvArgs for a result of another operation than the request's. A refusal changes nothing, writes nothing in the `ReplyBuffer`, and takes nothing from the result: a VMO handle in it is still the caller's, and the cookie of an `Opened` is still the handler's to release (it made the object; the core never heard of it). An accepted call moves the handle into the reply, whose write consumes it, and makes the connection for the cookie, which `release()` ends. The overload is the operation's own, so the result type and the request's operation are compared at run time only by the second refusal. Four overloads, not a variant parameter: `ustl::err(...)` converts to every `Result` and a variant of them would be ambiguous; a caller names the type, as the tests do.
- **A ticket is good once the operation that got it has returned `Pending`, not before.** The core marks the connection pending after the handler's call returns. A handler that completes inside its own call is refused (NotFound); it answers `Done` instead. The alternative, marking before the call, would let `complete()` return an `Outcome` to the handler's own stack, where nobody writes it. The test pins the rule.
- **`Pending` must hand back the ticket it was given.** A handler that promises another is answered `InternalError` (for an Open, an epitaph) and nothing waits: nobody could end the request it will wait for.
- **`release(cookie)` is called once for every cookie the handler made.** A connection that ends in any way (Close, the peer going, the program releasing it, the Drop of a message it should not have sent) tells the handler after the slot is freed, so the handler never meets a half-gone connection and a second `release()` finds a stale id. A cookie that never became a connection is released too: when check 9 refuses the kind the handler reports, and when the table is full (an Open can only find that out after the handler answered). A Clone asks for the slot first and the handler second, so there is never a cookie to give back for want of room; a refusal from the handler frees the slot without telling the handler, which has made nothing.
- **`dispatch()` on a pending connection is `Drop`, and the Drop carries the held server end.** The message is the program's bug (it must not read such a connection). The Open's server end the core held would leak with the connection, so `Outcome::handle` is the end for a `Drop`, and `release()` returns it too (`Uandle`, ignorable; Invalid unless an Open was pending). Step 9's glue must close it.
- **EXECUTE on a handler's file is the handler's to refuse.** The core cannot know whether a file the handler opened may be executed; the tree knows (`VmoFile::executable`). The rest of check 9 (the kind against the flags, FileAppend and FileTruncate, no write on a file) is the core's and shared with the tree's nodes through `judge_directory()` and `judge_file()`.
- **`Action::Pending` for the request that waits, no handle.** The program is told to leave the connection unread (a Loop callback will answer `Suspend`). A test with a `LoopServer` cannot reach the case in `loop_server.cpp` (it has no handler), so the one case there is defensive: release the connection and close the end it held.

**Change**
```cpp
// tree.hpp
enum class NodeKind : u8 { Directory, File, Service, Remote, Subtree, Handler /* new */ };
auto Tree::add_handler(NodeId parent, std::string_view name, u64 value) -> Result<NodeId, Status>;
auto Tree::handler(NodeId node) const -> Option<u64>;
```
```cpp
// server.hpp
struct Ticket { ConnectionId connection; u32 serial; };
template <typename R> struct Done { R result; };
struct Pending { Ticket ticket; };
template <typename R> using Answer = std::variant<Done<R>, Pending>;

class NodeHandler {
  public:
    virtual auto open(u64 value, Option<u64> directory, std::string_view path, Flags flags, Ticket) -> Answer<OpenResult> = 0;
    virtual auto read(u64 value, u64 cookie, u64 offset, u64 count, Ticket) -> Answer<ReadResult> = 0;
    virtual auto read_dirents(u64 value, u64 cookie, u64 cursor, u64 max_bytes, Ticket) -> Answer<EntriesResult> = 0;
    virtual auto get_backing_memory(u64 value, u64 cookie, VmoFlags flags, Ticket) -> Answer<VmoResult> = 0;
    virtual auto clone(u64 value, u64 cookie) -> Result<u64, Status> = 0;
    virtual auto release(u64 value, u64 cookie) -> void = 0;
  protected: /* as Handler: defaulted copies, a protected non-virtual destructor */
};

Server(Tree const &, std::span<Connection>, Syscalls, NodeHandler *handler = nullptr);
auto Server::complete(Ticket, OpenResult const &, ReplyBuffer &) -> Result<Outcome, Status>;  // and Read, Entries, Vmo
auto Server::release(ConnectionId) -> Uandle;   // was void
```
`Server::open()` before and after, at the end of the walk (checks 1 to 7 and the walk are unchanged):
```cpp
// before                                            // after
if (kind == NodeKind::Subtree) { ... }              if (kind == NodeKind::Subtree) { ... }
                                                    if (kind == NodeKind::Handler) {
                                                        return ask_open(id, connection, node, ustl::NONE, found->rest, flags, request.object);
                                                    }
// 9. directory / file rules written out here       // 9. judge_directory() / judge_file(), shared with finish_open()
```
and the request of a Read on a handler's file:
```cpp
auto const ticket = issue(id, connection);                       // ++serial
auto const answer = m_handler->read(value, cookie, connection.m_seek, count, ticket);
// Done: finish_read() copies the bytes, moves m_seek by their count, builds the reply
// Pending: park() records {Read, txid, count}, answers Action::Pending; m_seek does not move
```

**Reference**
Fuchsia's vfs has both shapes the design combines: a node trait whose operations wait, `Directory::open()` and `Directory::read_dirents()` (`src/storage/lib/vfs/rust/src/directory/entry_container.rs:77`, `:102`) and the file's `read_at()` (`src/storage/lib/vfs/rust/src/file/connection.rs:70`), and the C++ `Vnode` with `Read()` (`src/storage/lib/vfs/cpp/vnode.h:233`), `Readdir(VdirCookie *, ...)` (`:265`, whose cookie is this change's cursor), `GetVmo()` (`:191`) and `Close()` (`:225`). There `Poll::Pending` and the executor are the waiting; here `Done`/`Pending` are written by hand and `Action::Suspend`/`Loop::resume()` (step 7) leave the message in the channel. The ticket and its serial are this project's own: there is no counterpart because a future is its own token.

**Verification**
- Host tests, all pass: `ioservice` 138 (was 75), `loop_server` 32 and `outgoing` 63 (unchanged); the host executables are 166, as at the baseline. The new ones:
  - `Tree` (11): the node's kind, value (zero and a wide one) and name; its place in a listing; nothing added below it; the adders' refusals and the room left after them; `resolve()` stopping at it, the rest `.` for the node itself, for one segment and several, from a directory further down, as a view of the path, no segment after it looked at, and the first of a Handler node, a Subtree node and a remote entry winning; the refusals up to the node; `lookup()` reaching it and not going through it.
  - `Server`, each of 14 run once with a handler that answers at once and once with one that answers Pending and is ended by `complete()` before the test reads the reply: the handler is told the value, the rest of the path, the flags and the ticket, and the connection it makes has exactly the rights asked (a file's shrunk); an Open through a directory the handler opened is told its cookie and the whole path; fifteen Opens that checks 1 to 7 and the walk refuse never reach the handler, and the same checks on the directory the handler opened; the kind the handler reports against the flags (NotDir, NotFile, BadType, the file flags a directory refuses, no write on a file) with every refused object released; Execute on a file the handler refuses; byte-for-byte equal replies to a tree of the same content for Read in pieces, past the end and above the limit, for GetBackingMemory with five flag sets, and for ReadDirents at once and cut at every size (10, 0, 11, 12, 28, 15, 16, 16 and others); a Read without READ_BYTES, a ReadDirents without ENUMERATE, a count or byte limit above the limit, a first listing with no room for `.`, and a GetBackingMemory asking for rights the connection lacks all refused without asking the handler; the offset the handler is given and how it moves (by the bytes of a Read, not the count; not on an error); the `.` entry, the room the handler is given for the rest and the cursor it gets back; a Clone's cookie, its refusal, and the table full (the handler not asked); a full table refusing an Open with its cookie released; a refused Clone not keeping its slot; the release of every cookie once, by Close, by `release()`, by a stale id (nothing) and by a Drop; a handler that returns too many bytes, too many dirents or an invalid VMO answered InternalError with nothing moved.
  - `Server`, 21 with a handler that answers Pending and the test ending the requests: the Pending outcome, the ticket's connection and serial, the offset not moving until the completion and by its bytes alone; an Open held until it completes (Bind with the held end; Refuse with an epitaph for a handler's refusal, for a kind the Open's own flags refuse and for a table that filled up meanwhile, the cookie released each time, and no path in the Outcome); a message for a pending connection dropped and released, with the held server end in the Drop; the stale generation with the same serial on the connection that has the slot now; an earlier serial after a later request began; a serial bumped by a request answered at once; a ticket completed twice; a ticket of a request that never waited; a ticket of a released connection; a result of another operation; a refused completion leaving the `ReplyBuffer` and a VMO handle untouched (the buffer is marked before the call and compared after) and an accepted one putting the handle in the reply; two connections waiting at once and ended out of order with their own txids; a pending ReadDirents with the `.` and the cursor; a handler that promises another ticket; a completion inside the call that got the ticket; the release of a pending connection once.
  - `Server`, 3: a Handler node listed as a directory and refusing `bind()`; a synchronous handler's Open never waiting; a `Server` with no handler answering NotSupported.
- **Mutation checks of the sources, 62**, each a change to `server.cpp` or `tree.cpp` built, run and reverted (the script is in the session scratchpad, not in the tree; the tree was compared with a copy after every run and was identical). 60 fail named tests; 2 trap the test binary in the core (an `Option` of `Tree::handler()` dereferenced on a node that is not a Handler node, which is the same detection by another road). Every one of the 63 new tests fails under at least one of them. The counts are of failing tests in `ioservice`:

  | Mutation | Failing tests |
  |---|---|
  | check 1, 2 or 3 removed | 3 / 7 / 10 |
  | checks 6 and 7 made after the handler is asked; skipped for an Open on a handler's directory | 6 / 2 |
  | a file judged as a directory; a directory as a file; a directory's role a file's | 6 / 19 / 15 |
  | the cookie not released when check 9 refuses; when the table is full | 3 / 3 |
  | ReadBytes check removed; transfer limit removed; Enumerate removed; the GetBackingMemory flag check removed | 4 / 4 / 4 / 4 |
  | a pending Read advances the offset by the count at once; a completed Read does not advance it; advances by the count; the handler is given offset 0; a failed Read moves the offset | 9 / 9 / 3 / 9 / 4 |
  | the serial not bumped; not compared; the generation not compared (`find()`); a spent ticket completing again (`m_waiting` not cleared); the operation not compared; nothing pending accepted | 3 / 2 / 2 / 22 / 2 / 4 |
  | `Pending` with another ticket accepted | 1 |
  | the VMO reply built before the ticket is checked; the Read reply built before | 2 / 6 |
  | `release(cookie)` never called; called twice; called for a plain node connection | 7 / 7 / 5 |
  | the held server end not handed back on release; a message for a pending connection served | 2 / 1 |
  | the Open's flags not kept; the server end not held; the Handler node not kept (traps); no directory told for a nested Open; the refusal at completion not an epitaph | 12 / 17 / trap / 2 / 4 |
  | Clone asks the handler before the slot; a refused Clone's slot not freed; the clone shares the cookie; the clone is a plain node connection (traps) | 2 / 2 / 2 / trap |
  | no room set aside for `.`; the cursor not kept; the phase not advanced; `.` written every time; a first entry that does not fit not told; an oversized listing accepted | 3 / 5 / 5 / 4 / 4 / 2 |
  | oversized Read bytes accepted; an invalid VMO accepted; the value passed to the handler is 0 | 3 / 2 / 15 |
  | `resolve()` does not stop at a Handler node; a Handler node listed as unknown; `handler()` answers for any kind; `add_handler()` adds a Subtree node | 46 / 3 / 1 / 51 |
  | seven more in the tree, run against the Handler node tests alone (a Handler node linked first; something added below it; a name added again; a name not judged; every walk ends with a rest; a Handler node a place to start from; `lookup()` through it) | 9 / 1 / 1 / 1 / 1 / 2 / 1 |

  The first run of the mutations showed what the tests then did wrong: some of them read a refusal with `unwrap_err()`, or indexed a recorded call that was never made, and a mutation that turned a refusal into a success trapped the whole binary at the first such test, so the later tests never ran. The records of calls (`Log`) now fail the test on an index past the end, and a refusal is read through a helper that answers Ok for a success.
- `boot_check.py --out out/dc-step8` on the final tree, against the baseline of `5e44458d` measured first, in a worktree of its own, with the same command:

  | | Baseline | Now |
  |---|---|---|
  | dbg | PASS, 46 passed, 5 skipped, 0 failed, 0 error lines | the same |
  | rel | PASS, 44 passed, 8 skipped, 0 failed, 7 of 7 required, 0 error lines | the same |
  | host tests | PASS, 166 passed (executables), 0 failed | the same |

- Sizes, from `sizeof` on the host and the `OUTGOING_STORAGE` symbol of the user `files_server` ELF (they agree): `Connection` 40 to 80 bytes; `Outgoing::Storage` 13,320 to 14,600 bytes (+1,280, 32 connections). The README's 13,064 did not match the tree it described, whose `Node` is 80 bytes. The Loop's slot table follows `MAX_CONNECTIONS` and is unchanged.
- clang-tidy (`prebuilt/clang/llvm-23`, `-p out/dc-step8`, `--header-filter` on `system/lib/ioservice`) on `server.cpp`, `tree.cpp`, `loop_server.cpp`, `server_test.cpp` and `tree_test.cpp`: no diagnostic after the test fixes (it first named two member names and two missing `[[nodiscard]]` in the tests). `format_cpp.py --check` clean on every changed file.

**Deliberately not done**
- Everything of step 9: `LoopServer` answering `Suspend` for a `Pending` outcome, its `complete()`, the `Binding` keeping its key, `Outgoing::add_handler()`, and `examples/files`. `LoopServer` gives its `Server` no handler, so nothing reaches the one defensive `case Action::Pending` that the switch needed.
- Pipelining, a timeout, cancellation: one request per connection, as the design says.
- A lent buffer for a synchronous Read (repair item 2) and a smaller `Connection` (repair item 1): the first needs a measurement and the user's decision, the second a real filesystem's numbers.
- A refusal inside the handler's tree is not said on the debug console: `Outgoing::open_refused()` is step 9's, and a completion that refuses has no path to say (the Open's bytes are gone).
- Any change to the guide or the architecture notes; the passages are in the task note.

**How it was found**
None. A new feature, from the design. Three things the design left open and this step settled: the value on every call, the cookie on an Open through a handler's directory, and the extra pending state.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. A ticket is the connection's id and a serial that every request bumps, the ones answered at once too. What does `complete()` compare, and which two stale tickets does the serial catch that the generation of the id does not?
2. A refused `complete()` takes nothing from its result. Who closes a VMO handle in it, who releases the cookie of an `Opened` in it, and why can the core not do either?
3. Why does the handler get the cursor and the byte limit while the `.` entry stays with the core, and what does the reply look like when the first entry after `.` does not fit?

## Title: Server::complete answers a status it cannot encode instead of releasing the connection (2026-11-04)

**Files**
- `system/lib/ioservice/src/server.cpp` — `handler_refusal()`: a status the handler refuses with that `ours::Status` does not name becomes `InternalError`, applied where the core takes a handler's error status (`finish_open`, `finish_read`, `finish_read_dirents`, `finish_backing_memory`, and `clone`'s cookie). `answer()` is split: `reply_or_status()` builds the reply or the status form and returns a `Result`; `answer()` (for `dispatch()`) still drops the connection when even that fails; the three non-Open `complete()` overloads call `reply_or_status()` and never `answer()`
- `system/lib/ioservice/include/ioservice/server.hpp` — the doc comments of `Action::Drop`, `Server::complete()`, `OpenResult` and the private `answer()` / `reply_or_status()`
- `system/lib/ioservice/test/server_test.cpp` — 6 tests more (138 to 144): one per `complete()` overload and one that runs with both kinds of handler; the fake handler gets a `refusal` hook
- `system/lib/ioservice/README.md`, `.agents/tasks/directory-capability-step8/RESULT.md` — the claim that `complete()` may answer `Drop`, corrected

**Symptom**
Found by an independent review of the step 8 commit (`3564dc20`), and confirmed by three reviewers who tried to refute it. A `NodeHandler` that completes a Read, ReadDirents or GetBackingMemory with an error status that `ours::Status` does not name (a value from `Status::MaxCount` up) makes `Server::complete()` release the connection from inside the handler's own Waiter, answer `Drop`, and leave the connection's Loop slot suspended. The next `Bind` takes the freed index and rewrites the `Binding` that the suspended slot also points at; when the program `resume()`s the old slot its packet is handled as the new connection's, and the PEER_CLOSED of the old client releases a live connection and its cookie. For an Open the same status did something else, found while checking: `refuse()` ends in `wire::encode_epitaph(...).unwrap()`, which traps, so the program died. Both need a handler with a bug, but the core already turns the other bugs of a handler (too many bytes, too many dirents, an invalid VMO, a promised ticket that is another) into `InternalError`, and this one must not be the exception.

**Cause**
Two mistakes that need each other.

1. **A release outside the connection's own callback races with the reuse of its index.** The design (`.agents/tasks/directory-capability/DESIGN.md`, "When the client goes away while the request is pending", rule 4) says a connection is released only by its own callback, or by `on_rearm_failed` after the Loop freed the slot, because a `Binding` is one per connection index and is registered again only after the Loop has freed the slot. The step 8 code kept the rule for `complete()`'s success path and broke it on one failure path: the three non-Open overloads ended in `answer()`, which for a reply it cannot encode calls `drop(id)`, which calls `release(id)`. In `dispatch()` that is harmless, because `dispatch()` is the connection's own callback. In `complete()` it is not: `complete()` runs in the handler's Waiter while the slot is suspended, and the result said `Ok(Outcome{Drop})`, so the program had no reason to treat the connection as anything but already gone. The step 8 note that "the connection is already released by the core, so it must be resumed" is not enough: the `resume()` happens after the handler's Waiter returns, and any `Bind` in between takes the lowest free index, which is the one just freed (`allocate()` scans from 0).
2. **A status the handler returns was trusted to be encodable.** `wire::encode_status_reply()` and `encode_epitaph()` refuse a status `ours::Status` does not name (`names_a_status`, `value < Status::MaxCount`), because a peer's status is a plain u32 on the wire and the receiver switches over the enum. The core read the handler's `Status` and passed it on as if only the core could have made it. So the only way `answer()` could fail, which nobody had thought of as a way to fail at all, was reachable from a handler. For the Open the same trust sat in `refuse()`, whose comment said "every status passed here is one ours::Status names" and whose `unwrap()` made the comment a trap.

The fix for the first mistake alone (make `complete()` return an error and not release) would leave a client hanging for a reply the core could not build. The fix for the second alone (sanitise) would leave a path in `complete()` that can release if a different encode failure is ever added. Both are made.

**Change**
The status a client is told is made at the one place the core reads a handler's error, so `dispatch()`'s `Done` and `complete()`'s `Pending` give the same reply:

```cpp
// before: finish_read, finish_read_dirents, finish_backing_memory, finish_open
if (result.is_err()) {
    return ustl::err(result.unwrap_err());
}

// after
if (result.is_err()) {
    return ustl::err(handler_refusal(result.unwrap_err()));
}

auto handler_refusal(Status const status) -> Status {
    if (std::to_underlying(status) >= std::to_underlying(Status::MaxCount)) {
        return Status::InternalError;
    }
    return status;
}
```

`InternalError` is the status the core already answers for a handler that returns too many bytes or dirents, an invalid VMO or a ticket it was not given, so there is one fallback and a client sees one thing for "the handler is broken". The same call is made on `clone()`'s refusal, where `refuse()` would have trapped the same way.

`complete()` can no longer reach a release, by construction and not only by the argument above:

```cpp
// before: the three non-Open overloads
return ustl::ok(answer(ticket.connection, prot::io::READ, request.txid, finish_read(...), out));

// after
return reply_or_status(prot::io::READ, request.txid, finish_read(...), out);

auto Server::reply_or_status(...) -> ustl::Result<Outcome, Status>;   // Err only if the status form cannot be built
auto Server::answer(...) -> Outcome;   // dispatch() only: reply_or_status(), and drop(id) on an Err
```

Whether `reply_or_status()` can still fail from `complete()`: the method is two-way, the txid is the decoded request's (it passed `decode_request_header`, which is the check `encode_status_reply` repeats), the `ReplyBuffer` holds a whole message, and the status is now one the codec names. So it cannot, and there is no test for the branch. If it ever could, `complete()` returns the codec's status (`InvArgs` or `BufferTooSmall`), the request is ended, the connection is alive and nothing is written, which `Server::complete()`'s comment says. The Open overload already did not release: it refuses with an epitaph on the server end it held, and the connection the Open came on is untouched; what was wrong there was the trap, which `handler_refusal()` removes. `dispatch()` keeps `answer()`'s `drop(id)` for an encode failure, and a handler's unnamed status no longer reaches it: `dispatch()` with a `Done` that carries one now answers `InternalError` instead of dropping (and the Open, instead of trapping).

Rejected: a distinct fallback status (nothing for a client to do with it), returning `Err` from `complete()` for an unnamed status (the handler cannot do anything with it, and the client would wait for a reply that nothing sends), and a `wire::names_a_status()` exported from the codec (a second library to change for a comparison against `Status::MaxCount`, whose own comment says it is the bound).

**Reference**
None, this project's own design: the Loop's `Binding` per connection index and the rule that a connection is released only by its own callback are decisions of `.agents/tasks/directory-capability/DESIGN.md` (rule 4 of "When the client goes away while the request is pending"). The shape of the fix is the one the core already used for the other handler bugs (`finish_read`'s `InternalError` for more bytes than asked).

**Verification**
- Baseline measured first, at `dd73ea94`, with `boot_check.py --out out/dc-step8fix` in the worktree the change was made in; the final tree with the same command:

  | | Baseline | Now |
  |---|---|---|
  | dbg | PASS, 46 passed, 5 skipped, 0 failed, 0 error lines | the same |
  | rel | PASS, 44 passed, 8 skipped, 0 failed, 7 of 7 required, 0 error lines | the same |
  | host tests | PASS, 172 passed (executables), 0 failed | the same |

  (the baseline moved from 166 to 172 executables since step 8; the change adds none)
- Host: `ioservice` 144 tests (was 138), `loop_server` 32 and `outgoing` 63, unchanged. The new ones: for each of Read, ReadDirents, GetBackingMemory (a Pending request completed by `complete()` with each of four unnamed statuses: `Status::MaxCount`, one past it, 1000 and 0xFFFFFFFF) the reply is a status reply carrying `InternalError`; the connection is alive; the handler was not told it ended; a `bind()` afterwards takes another index than the connection's; the connection then serves the next request (a Read from offset 0, a listing from `.`, a VMO); and `release()` from its own callback tells the handler once. For the Open the Outcome is `Refuse` with an epitaph carrying `InternalError` on the held server end, and the directory connection it came on is kept and serves the next Open and ReadDirents. The sixth runs for a handler that answers at once and for one that is completed: all four operations and a Clone refused with each unnamed status, nothing released, the connections serving afterwards.
- **Mutation**, the new tests run one at a time (`--gtest_filter`), then the whole binary; the script is in the session scratchpad, the tree was compared with a copy after the run and was identical:

  | Mutation | Read | ReadDirents | GetBackingMemory | Open | both-handlers test (2) | whole binary |
  |---|---|---|---|---|---|---|
  | `server.cpp` as it was at `HEAD` | fail | fail | fail | trap | trap, trap | trap |
  | `finish_read` passes the status on | fail | | | | fail, fail | 3 failed |
  | `finish_read_dirents` passes it on | | fail | | | fail, fail | 3 failed |
  | `finish_backing_memory` passes it on | | | fail | | fail, fail | 3 failed |
  | `finish_open` passes it on | | | | trap | trap, trap | trap |
  | `clone` passes the cookie's status on | | | | | trap, trap | trap |
  | `complete(Read)` through `answer()`, `finish_read` passes it on (the defect, one overload) | fail | | | | fail, fail | 3 failed |
  | the same for `complete(ReadDirents)` | | fail | | | fail, fail | 3 failed |
  | the same for `complete(GetBackingMemory)` | | | fail | | fail, fail | 3 failed |
  | the fallback is `BadState` | fail | fail | fail | fail | fail, fail | 6 failed |
  | `Status::MaxCount` itself let through | fail | fail | fail | trap | trap, trap | trap |

  A blank cell passed. The traps are the `unwrap()` of `refuse()`, which ends the test binary and not one test (each was run alone, so the others are known). The step 8 mutation records stand: no existing test changed.
- clang-tidy (`prebuilt/clang/llvm-23`, `-p out/dc-step8fix`, headers of `system/lib/ioservice` included) on `server.cpp` and `server_test.cpp`: no diagnostic. `format_cpp.py --check` clean on the three C++ files; `lint_docs.py --changed` clean.

**Deliberately not done**
- `answer()`'s `drop(id)` for a reply that cannot be encoded stays in `dispatch()`: it is the connection's own callback, where a release is right, and the case is unreachable now. It was not turned into a trap.
- `LoopServer` is untouched: it still gives its `Server` no handler, and the `Drop` case of its `handle()` is right for `dispatch()`. Step 9's glue for `complete()` can rely on never being handed `Drop` (RESULT.md says so).
- `Syscalls::handle_duplicate`'s status in `get_backing_memory()` (the tree's file) is not sanitised: it is the program's function, a kernel status, and only `dispatch()` reads it.
- A test of the `Err` branch of `reply_or_status()` in `complete()`: it needs a txid or a buffer the core never builds.

**How it was found**
An independent review of `3564dc20` read `complete()` against the design's rule 4 and traced `answer()` from the three overloads: `drop(id)` calls `release(id)`, and `release()` frees the index and calls the handler. Three skeptics tried to refute it by finding a reason `answer()` could not fail from there, and found instead that `encode_status_reply` fails for any status `ours::Status` does not name, and that `names_a_status` is the bound. Checking the Open overload for the same flaw found the trap in `refuse()`.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. `dispatch()` and `complete()` both ended in `answer()`, which drops the connection when it cannot build a reply. Why is that right in one and wrong in the other, and what does the Loop do with the index between the release and the `resume()`?
2. Why does the fix make the status safe where the core reads the handler's error instead of making `complete()` return an error for it, and which client would wait for ever under the second choice?
3. A refused Open is an epitaph on the server end the core held. Why did the same unnamed status trap there and release here, and what is it about `unwrap()` on `encode_epitaph` that made a comment the only guard?

## Title: A handler that answers Pending is suspended and completed on the Loop, and Outgoing serves a declared path through a NodeHandler (2026-11-04)

**Files**
- `system/lib/ioservice/include/ioservice/loop_server.hpp`, `src/loop_server.cpp` — `LoopServer` takes the program's `NodeHandler` as its last constructor argument and gives it to its `Server`; `Action::Pending` makes the `Binding` answer `loop::Action::Suspend`; the `Binding` keeps the key `Loop::add()` answered (`m_key`; 24 to 32 bytes); `LoopServer::complete(ticket, result)`, four overloads, carries out what `Server::complete()` answers and resumes the connection's slot; the `Drop` case closes the handle it carries; the `Bind` and `Refuse` cases are shared with `complete()` (`bind()`, `refuse()`); every release goes through a private `release()` that closes the server end of an Open that was pending on the connection
- `system/lib/ioservice/include/ioservice/outgoing.hpp`, `src/outgoing.cpp` — `Outgoing::add_handler(path, handler, value = 0)`; `Outgoing::complete()`, four overloads forwarding to the `LoopServer`; `Outgoing` is the `LoopServer`'s `NodeHandler` (a second private base beside `Delegate`) and passes each call to the handler registered for the node; `HandlerEntry` gains `node` and `node_value` (16 to 32 bytes); `MAX_PROTOCOLS` now limits four kinds of registration
- `system/lib/ioservice/test/loop_server_test.cpp` — 17 tests more (32 to 49): a Handler node `/fs` answered by a fake `NodeHandler`
- `system/lib/ioservice/test/outgoing_test.cpp` — 21 tests more (63 to 84): 16 for `add_handler()` and the forwarding, 5 more rotations of the shared table of eight
- `system/lib/ioservice/README.md` — pending requests, `add_handler()`, the shared table, the storage size
- `docs/repairs/261103/ioservice.md` — item 1 gains the two sizes above

**Symptom**
Latent until `examples/files` uses it (`docs/changes/261103/examples-files.md`): before this change no `LoopServer` gave its `Server` a handler, so the one `case Action::Pending` of `handle()` was defensive and released the connection if it were ever reached. This is step 9 of 9 of `.agents/tasks/directory-capability/DESIGN.md` (decision 7, option C′, the glue and the first use). Step 7 gave the Loop a slot that can be left unarmed (`Suspend`, `resume`) and step 8 gave the core a request that can wait (`Pending`, `Server::complete`); nothing yet put the two together or let a program register a handler.

**Cause**
The pieces of C′ are three, and each fails alone:

- **A pending request has to leave its connection unread.** `Server::dispatch()` answers `Pending`, and the connection's next message is already in the channel; a slot the Loop re-arms (`Rearm`) is called with it at once, while the first request is unfinished, and the core drops the connection (`dispatch()` on a pending connection is `Drop`). `Suspend` leaves the message where it is, in the kernel's channel, in order, and costs the server nothing.
- **The answer has to be written before the slot is resumed**, and the slot resumed with the key it was given. The `Binding` dropped the key `add()` answered, since a slot that answered only `Rearm` or `Close` needed none. A `resume()` with a guessed key resumes the wrong connection or none, and the connection stays suspended for good.
- **A connection is released only by its own callback.** The design's rule 4 of "When the client goes away while the request is pending": `complete()` runs in the handler's own Waiter, while the connection's slot is still in use and points at the connection's `Binding`. A connection released there frees its index, a new connection takes the index and the same `Binding`, and the old slot's late packet is then handled as the new connection's: its PEER_CLOSED would release a connection that is alive. So a reply that cannot be written (the client went away while the request waited) is told to `write_failed()` and nothing more; the slot is resumed, PEER_CLOSED is observed in the connection's own callback, which releases the connection, calls `release(cookie)` on the handler once and answers `Close`.

Decisions the design left to this step, in the order a reader meets them:

- **`complete()` returns a `Status`, and Ok means the Server took the result.** NotFound or InvArgs are the Server's refusals, with nothing written and nothing taken from the result (a VMO handle or an `Opened` cookie is still the handler's). A write that fails is not an error of `complete()`: the result is the reply's now, and `write_failed()` says what happened. BadState or NotFound from the resume are returned, because they mean the slot was not suspended, which is a bug of the program and not of the handler; any other status from the resume is the kernel refusing the arm, which the Loop has dealt with (the slot freed, the `Binding` released the connection) before `resume()` returned, so the answer is Ok.
- **A completion cannot run inside the Binding's own callback, so there is no `Rearm` branch.** Step 7 noted that `Loop::resume()` from inside the callback of the slot being resumed is BadState, and asked for the case to be handled: if `complete()` could run inside the callback, the `Binding` would have to answer `Rearm` and not `Suspend`. It cannot: `Server::complete()` accepts a ticket only once the handler's call has returned `Pending` (step 8: the ticket is good after the call, not before), and nothing runs between that return and the `Binding`'s own, so a handler that completes inside the call it was given is refused with NotFound and answers `Done`. A test pins it (`ACompletionInsideTheCallThatGotTheTicketIsRefusedAndTheConnectionIsServed`); the `Binding` answers `Suspend` for every `Pending`, with no flag to remember a completion. The neighbouring case that can happen is tested too: someone else resumed the slot while the request waited, so the resume in `complete()` answers BadState, which `complete()` returns after the reply is written.
- **A completed refusal is not reported to `open_refused()`.** It has no path: the Open's bytes are gone by the time the handler answers. The line is for the outgoing root and `svc/` only anyway (`Outgoing::open_refused()`), and a refusal found at completion is under a Handler node. Documented on `Delegate::open_refused()` and `Outgoing`.
- **`Drop` carries a handle to close**, as step 8's note asked: a message for a pending connection is dropped, and the Open's server end the `Server` held with it is closed by the glue. The same server end is closed when a connection is released while its Open is pending (a private `LoopServer::release()` wraps every release); it is `Invalid` in every callback that runs today, so both are exercised in the tests by resuming a pending connection's slot by hand, as a program that broke the rule would.
- **`add_handler(path, handler, value = 0)`.** Like `add_subtree(path, handler)` in everything that judges the path (InvArgs, AlreadyExists, NoResource, judged whole before any node is made, so a refusal leaves nothing behind) and in the shared table. It differs in one thing: it takes the `value` the handler is told on every call about the node. The `NodeHandler` contract (step 8) says the value tells the handler which of its nodes a call is for; the table index the node is registered under is the `Outgoing`'s own number, and showing it would make the value depend on the order of registrations. `HandlerEntry` therefore keeps the program's value beside the handler. The default is 0 for a handler that serves one node.
- **`Outgoing` is the `LoopServer`'s `NodeHandler`, by a second private base.** The `Server` asks one handler about every Handler node of its tree, and finds the node by the value the tree holds, which here is a table index: the lookup belongs to the class that owns the table, and `Delegate` is the precedent (the same class already is the `LoopServer`'s delegate, for the same reason). It is the implementation of an interface the `Server` requires and not a new hierarchy, and a nested helper class would need the table's size and storage by reference to do what `Outgoing` does with its own members; if the maintainer prefers the helper, the six overrides move into it and nothing else changes.
- **`Outgoing::complete()` is four forwarding overloads**, the smallest interface that lets a program end a request without reaching the `LoopServer` (which `Outgoing` keeps private, as it keeps the tree). A template or a getter for the `LoopServer` would be smaller still and would give a program the whole `serve()` surface.
- **The glue does not own the delay.** A handler decides when to complete; the library has no timer (the Loop has none). The rule from the design stands: a handler that answers `Pending` completes, on the answer or on the failure of what it waits for.

**Change**
`LoopServer::handle()`, before and after, for the case that mattered:

```cpp
// before: unreachable, defensive
case Action::Pending: {
    auto const held = m_server.release(connection);
    if (held != Uandle::Invalid) { auto _ = ours::handle_close(held); }
    return loop::Action::Close;
}

// after
case Action::Pending: {
    // The NodeHandler will answer later, through complete(). ...
    return loop::Action::Suspend;
}
```

`watch()` keeps the key, `complete()` ends a request:

```cpp
// watch(), after the Loop took the channel
binding.m_key = added.unwrap();

auto LoopServer::complete(Ticket const ticket, ReadResult const &result) -> Status {
    return finish(ticket, m_server.complete(ticket, result, *m_memory.reply));   // and Open, Entries, Vmo
}

auto LoopServer::finish(Ticket const ticket, ustl::Result<Outcome, Status> const &completed) -> Status {
    if (completed.is_err()) { return completed.unwrap_err(); }        // NotFound, InvArgs: nothing written or taken
    auto const outcome = completed.unwrap();
    auto const channel = *m_server.channel(ticket.connection);          // alive: the Server took the ticket
    auto const key = m_memory.bindings[ticket.connection.index].m_key;
    if (outcome.action == Action::Reply) {
        if (auto const written = write(channel, outcome.message); written != Status::Ok && m_delegate != nullptr) {
            m_delegate->write_failed(written);                          // and the connection is left alone
        }
    } else if (outcome.action == Action::Bind) {
        bind(outcome);                                                  // watch(), or an epitaph and a close
    } else if (outcome.action == Action::Refuse) {
        refuse(outcome);                                                // epitaph, close; not reported
    }
    if (auto const resumed = m_loop->resume(key); resumed == Status::BadState || resumed == Status::NotFound) {
        return resumed;                                                 // the slot was not suspended
    }
    return Status::Ok;
}
```

`Outgoing`:

```cpp
auto add_handler(std::string_view path, NodeHandler &handler, u64 value = 0) -> ustl::Result<NodeId, Status>;
auto complete(Ticket, OpenResult const &) -> Status;       // and Read, Entries, Vmo

struct HandlerEntry {                  // one place of the table, indexed by the node's value
    Handler *service = nullptr;        // a protocol, or a service node
    SubtreeHandler *subtree = nullptr; // a Subtree node
    NodeHandler *node = nullptr;       // a Handler node
    u64 node_value = 0;                // what add_handler() was given
};
```

Rejected: a `Rearm` flag in the `Binding` for a completion that already happened (dead code: the case cannot arise, and a test could only reach it by breaking a rule the Server enforces); making `complete()` release the connection when the write fails (the defect rule 4 exists for: mutation L6 below); a separate handler table for Handler nodes (the design says one table, and `MAX_PROTOCOLS` limits all four).

**Reference**
None, this project's own design: `.agents/tasks/directory-capability/DESIGN.md`, "Waiting for a reply: the handler node, Suspend and resume (C′)", the section on a client that goes away while the request is pending, and the Staging row 9. The shape of a handler operation that answers now or later follows the async operations of the Rust vfs (`src/storage/lib/vfs/rust/src/directory/entry_container.rs:102-107`), with `Done` and `Pending` written out where Rust has `Poll::Ready` and `Poll::Pending`.

**Verification**
- Host, `out/dc-step9`: `loop_server` 49 tests (32 before), `outgoing` 84 (63), `ioservice` 144 (unchanged), the whole suite 172 executables, all pass.
- **Mutation, `loop_server.cpp` (18) and `outgoing.cpp` (16), one at a time, each built, run and reverted** (the scripts are in the session scratchpad; the source was compared with a copy after every run). Every one fails a named test. **Every one of the 38 new tests fails under at least one mutation.**

  | Mutation of `loop_server.cpp` | Tests failing |
  |---|---|
  | Pending answers `Rearm` / `Close` instead of `Suspend` | 15 / 14 |
  | resume skipped / resume called twice | 9 / 11 |
  | the resume is made before the reply is written | 1 (`AResumeThatTheKernelRefusesReleasesTheConnectionAfterTheReplyIsOut`: the reply is lost when the arm fails) |
  | the key never stored | 11 |
  | `complete()` releases the connection when the write fails | 2 |
  | the write failure not told to `write_failed()` | 2 |
  | `release()` does not close the Open's held server end | 1 |
  | `Drop` does not close the handle it carries | 1 |
  | BadState and NotFound from the resume swallowed | 1 |
  | the `Bind` of a completed Open not carried out / the `Refuse` not carried out | 2 / 2 |
  | the reply written on another connection's channel | 1 (two pending connections completed in reverse order) |
  | `rearm_failed()` does not release | 3 |
  | the handler not given to the `Server` | 5 |
  | a refused ticket carried out as if accepted | 3 |
  | the PEER_CLOSED path does not release | 5 |

  | Mutation of `outgoing.cpp` | Tests failing |
  |---|---|
  | the table not judged / the index not advanced | 13 / 18 |
  | the table judged after the directories are made | 1 (`AddHandlerRefusedForTheTableLeavesNoDirectoryBehind`) |
  | the declared path not recorded | 2 |
  | the program's value not kept / the table index told in its place | 3 / 2 |
  | `read` forwards offset and count swapped / `clone` the wrong cookie / `get_backing_memory` the wrong cookie / `read_dirents` the wrong value | 2 / 1 / 1 / 1 |
  | `release` not forwarded / `complete(Read)` forwards nothing | 2 / 1 |
  | the node added as a Subtree node / the `LoopServer` not given the `Outgoing` as handler | 10 / 15 |
  | `judge()` accepts a relative path / accepts `/svc` | 5 / 5 (three of the five are `add_directory`'s and `add_subtree`'s) |

  The reply-before-resume order is invisible on one thread unless the resume's arm fails (the Loop then closes the channel the reply was to be written on), so `AResumeThatTheKernelRefusesReleasesTheConnectionAfterTheReplyIsOut` is the one test that holds it.
- **Target.** `routing_checks` reads the slow file, writes two Reads back to back and abandons 40, through this glue, on the real kernel (`docs/changes/261103/routing-checks.md`). Mutations of the library on the target: `Rearm` for `Pending` fails the back-to-back case (after the delay was made two turns, see `docs/changes/261103/examples-files.md`); `complete()` writing to the wrong connection fails the slow cases; a connection not released when its client is gone fails the 40-abandoned case with `NoResource`; no resume hangs the boot (`no "routing_checks: passed"`).
- Sizes, from the `OUTGOING_STORAGE` symbol of the user `files_server` (`llvm-nm -S`): 14,984 bytes, 384 more than the 14,600 of step 8: a `Binding` is 32 bytes where it was 24 (32 of them: 256), and a table entry 32 where it was 16 (8 of them: 128). The root's boot server has 128 `Binding`s: 1,024 bytes more. The Loop's table follows `MAX_CONNECTIONS` and is unchanged. Repair item 1 of `docs/repairs/261103/ioservice.md` now lists both.
- `boot_check.py --out out/dc-step9` on the final tree against the baseline of `e7bf44bd` measured first in the same worktree:

  | | Baseline | Now |
  |---|---|---|
  | dbg | PASS, 46 passed, 5 skipped, 0 failed, 0 error lines | the same |
  | rel | PASS, 44 passed, 8 skipped, 0 failed, 7 of 7 required, 0 error lines | the same |
  | host tests | PASS, 172 passed (executables), 0 failed | the same |

  Ten more rel boots with 4 CPUs (the default): 10 of 10 pass. Nine with `SMP=2`: 9 of 9 pass (the known `Gpt.GetInfo = PeerClosed` race of `docs/repairs/261103/routing-checks.md` did not appear). `SMP=1` does not boot on this tree (`CPU[0] tick device I8253 failed`, in the kernel before user space; no kernel source changed here, so it is not this change's).
- clang-tidy (`prebuilt/clang/llvm-23`, `-p out/dc-step9`, headers of `system/lib/ioservice` and `examples/files` included) on `loop_server.cpp`, `outgoing.cpp`, both test files, `files_server.cpp` and `routing_checks.cpp`: no diagnostic. `format_cpp.py --check` and `format_gn.py --check` clean; `lint_docs.py --changed` clean.

**Deliberately not done**
- The guide and the architecture notes, as the brief says. The passages step 9 makes wrong are in `.agents/tasks/directory-capability-step9/RESULT.md`.
- A `Rearm` branch for a completion inside the Binding's callback (above): the case cannot arise.
- Pipelining, a timeout and cancellation: one request per connection, as the design says. A handler whose wait never ends keeps its connection suspended for good.
- A test of `LoopServer::complete()` for an `EntriesResult` or a `VmoResult`: the fake handler answers those operations at once, and `finish()` is the same code for all four; the core's own tests (step 8) cover what each overload builds.
- The `Binding` and the table entry could be smaller (`m_connection` and `m_key` carry overlapping information); not measured to matter, listed in repair item 1 with the sizes.

**How it was found**
None, a new feature from the design. Two things only the target showed: the example's delay had to be two Loop turns and not one (`docs/changes/261103/examples-files.md`), and `routing_checks` is at the limit of its namespace (`docs/changes/261103/routing-checks.md`).

**The user's prediction**
None.

**Three questions you should be able to answer**
1. `complete()` writes the reply and then resumes the slot. Which request would be started too early if the order were reversed, and why can no test on one thread see it unless the resume's arm fails?
2. A client closes its channel while its Read is pending. Who sees it, when, and who calls `release(cookie)`? Why must `complete()` not release the connection when its write fails, though it is the first to know the client is gone?
3. The step 7 note says a completion inside the callback of the connection being completed would leave it suspended. Which rule of the core keeps that from happening, and what does a handler that tries it get back?
