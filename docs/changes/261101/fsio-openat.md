# fsio-openat changes

**Files**
- `system/lib/fsio/` — `open(Uandle dir, std::string_view path, Flags)`:
  the Open message `open(path, flags)` sends after its namespace hop, sent
  to the caller's directory handle instead, the path never
  namespace-resolved. The send half is factored out (`send_open`), both
  fronts share it. A path that cleans to a leading '/' or a ".." is
  BadPath; a dead directory handle closes the client end it made and
  returns the failure.
- `system/service/init/test/routing_checks/` — check_disk holds a writable
  /disk handle, open_at-creates a file under it, writes, reopens, reads the
  pattern back, unlinks; a "../x" open is BadPath without a message.

**Symptom**
None — a planned gap. The protocol's Directory.Open was always relative to
the connection it arrived on, and every server walked such paths; the
client library exposed only namespace-absolute opens, so a program holding
a directory handle could not say "this name, under this".

**Cause**
The client's open is two halves: which directory (the namespace's job) and
the Open message (the protocol's). The namespace answers the first with a
leading-'/' rule -- resolve refuses anything not starting with '/' -- so
the second half was unreachable on its own. The overload adds the missing
front: same message, directory chosen by the caller's handle. The refusal
of ".." past cleaning is deliberate: a directory connection has no above --
the namespace's mount points are not reachable from a handle -- and a
lexical fold across that boundary would promise an escape the capability
model does not carry. POSIX's cwd is built on this exact primitive (an
openat with a magic fd); the cwd itself is deferred until this lands.

**Change**
`open(dir, path, flags)`: clean the path (BadPath past MAX_PATH_LENGTH),
refuse leading '/' and leading "..", channel-create the pair, send
`Directory.Open` with the client's server end on `dir`, return the client
end. A refusal by the server (rights, NotFound) surfaces the way the
namespace open's does: the epitaph the server wrote onto the object end is
read at the handle's first use.

**Reference**
POSIX `openat(2)` with a dirfd, Fuchsia's `fdio_open_at` -- the same
shape: the directory handle is the capability, the path is relative to it,
and ".." past the root is the caller's error. The overload-not-new-name
choice keeps the API surface at one verb, as POSIX's open(2)/openat(2)
pair does in spirit (one open, the dirfd variant being the general form).

**Verification**
`boot_check.py`: dbg PASS (44), rel PASS (42, 6/6 required), host-tests
PASS 157 -- diskless, where /disk skips. With the GPT disk:
`routing_checks: passed` -- a writable /disk handle open_at-creates the
file, writes the pattern, a fresh relative reopen reads it back byte for
byte, unlink removes it, and "../x" is BadPath client-side. The check's
first draft also caught the rights model working: the read-only listing
handle's create was refused (AccessDenied via the epitaph at the first
write), which is why the open_at check holds a writable handle of its own.
clang-tidy quiet.

**Deliberately not done**
- Process cwd (chdir and relative fsio::open): deferred per the plan -- it
  is a thin shell over this overload once it exists, and its spawn-time
  inheritance touches the launcher, a different surface.
- ".." folding to the parent directory connection: the servers have no
  parent pointers, and a mount-point-crossing ".." is exactly what the
  capability model must not promise.

**How it was found**
None.

**The user's prediction**
None. The user picked the overload over a new name, and the ".." and cwd
defaults by silence on the recommendation.

**Three questions you should be able to answer**
- Why is ".." refused in the client rather than sent, and what would the
  server's answer have been if it were not?
- The check's first draft got AccessDenied at the first *write*, not at
  the open -- what was the open waiting for, and what does that say about
  when an Open's refusal becomes visible?
- Where in this design would a cwd handle sit, and what would break if two
  threads chdir'd concurrently?
