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
