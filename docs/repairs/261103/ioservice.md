# ioservice outstanding repairs

## 1. Every `Connection` is 80 bytes, twice what it was, and only a connection to a handler's object uses the new half

**Severity**
Low: a cost in bytes, not a wrong result. A provider's `Outgoing::Storage` is 1,280 bytes larger for it, in `.bss`, and the root's boot server (128 connections) 5,120.

**Symptom**
Measured on 2026-11-03 with `sizeof` on the host and the `OUTGOING_STORAGE` symbol of `files_server` (the two agree): `Connection` 40 to 80 bytes, `Outgoing::Storage` 13,320 to 14,600 bytes. The design (`.agents/tasks/directory-capability/DESIGN.md`, "Where it lives in the provider") listed four new fields and called their cost "not measured". The fields are those four (a cookie, a serial, the pending flag as `m_waiting`, and the Open's held server end) and four more that the design did not list: the role (what the connection is to), and, because the handler's answer does not carry them, the request's txid, its argument (an Open's flags, a Read's count, a ReadDirents' byte limit) and the Handler node an Open walked to.

**Cause**
The pending state is in every connection, though a connection waits for a handler only if its node is a Handler node's object or a directory an Open of one came on, and the cookie and the role matter to handled objects alone. A plain tree connection pays 40 bytes for fields it never reads. The request's 24 bytes are in use only while a request waits, one at a time per connection.

**Change**
Not decided, and not to be done before measuring what a real filesystem's table needs: if the connection counts stay at 32 and 128 the cost is 1.2 KiB and 5 KiB, and there is nothing to gain. Candidates if it matters: overlay the request with the tree directory's cursor (`m_next_entry`, 8 bytes) where the connection is not a Node directory, which saves little; move the request into a second table indexed like the connections, sized by the program that has handler nodes, which saves the 24 bytes everywhere else and costs a second array and a lookup; make the size of the table a template parameter (decision 8 (a) of the design). Ruled out for now: dropping the txid (the reply carries it), recomputing the Open's flags (the handler's answer does not carry them).

**Reference**
None, this project's own design.

**Acceptance**
A `static_assert` on `sizeof(Connection)` beside the new figure, and the `Outgoing::Storage` size in the README, move together; the `ioservice` tests pass unchanged.

## 2. A Read from a handler node copies the handler's bytes into the reply, one copy more than a handler needs when it builds them in memory it will not reuse

**Severity**
Low: one `memcpy` of at most 4,072 bytes per Read, and the same count as a file of the tree makes.

**Symptom**
`Server::finish_read()` copies the span a handler returned into the reply buffer. A file of the tree makes the same one copy (from its `VmoFile::bytes`), so the count is equal when the handler's bytes are kept in memory. A handler that builds the bytes for each Read (decompresses, decodes, reads a block into a buffer of its own) copies them twice: once into its buffer and once into the reply. Not measured.

**Cause**
A request that answers Pending cannot be given the reply buffer to write into: one `ReplyBuffer` serves every callback, so by the time the handler completes it holds another connection's reply. The one rule that fits both answers (the bytes are the handler's, valid until the core call that received them returns) therefore copies in the core for a synchronous handler too. Lending the buffer for a `Done` answer would save the second copy for a handler that builds its bytes, at the cost of a second rule and a handler that must know which kind of answer it is giving before it starts.

**Change**
Not decided. Measure first, on the target: Read throughput of a handler that builds its bytes, with the copy and without (a lent buffer for a synchronous answer), against a file of the tree. Ruled out for now: letting a pending handler keep a pointer into the reply buffer (it is overwritten by the next callback).

**Reference**
None, this project's own design. The design's "A file's VMO from a provider that serves bytes from a disk" is the case that makes the copy matter.

**Acceptance**
A report in `docs/perf/` with the baseline and the stage; the replies the `ioservice` tests compare byte for byte are still equal.
