# init changes

## Title: The root forwards a directory use that ends at a provider as one Open of its declared path (2026-10-31)

**Files**
- `system/service/init/realm.cpp` — `Realm::forward_directory()` is new; the branch of `Realm::run()` that answered `NotSupported` for a directory route that ends at a provider is gone; `forward()` is split into the two halves it shares with `forward_directory()`, `provider_serves()` (the exited-provider rule and its log line) and `write_open()` (encode, fire and forget, the "cannot forward" line); `to_flags()` turns a route's rights into the permission bits of an Open
- `system/service/init/realm.hpp` — the three declarations with their contracts, and the comment on `Instance::m_outgoing`

**Symptom**
Step 2 let a manifest declare, expose and offer a directory, and step 3 gave a provider's `Outgoing` a place to put one, but a consumer of such a directory still got nothing. `Realm::run()` routed the use, found a provider at the end, and closed the entry's server end with a `NotSupported` epitaph: every `open()` under that namespace entry answered `NotSupported`, and the root logged nothing, because it was not a routing failure but a branch that had not been written.

**Cause**
The root had one way to serve a directory entry, its own `/boot`: `m_boot->serve(server, subdir, rights)`. That was complete while design D3 allowed one directory, the built-in. A directory that a component provides cannot be served by the root. The entry is a channel pair (N, S): N goes to the consumer as its namespace entry, S is the end whoever serves the directory must hold. For `boot` the root holds S itself. For a provider, S has to get to the provider, and the one way into a provider is its outgoing directory, whose client end only the root holds. So the root writes an `Open` on that end with S as the request's object, which moves S into the provider's process, and from then on the consumer and the provider talk on (N, S) directly. The root keeps nothing for the entry: no connection, no Loop slot, no table entry. That is the property that matters (design invariant I1 and "After the start, the root holds nothing per directory entry"): the root is one thread that must never wait, and a filesystem will wait.

Each field of that Open is a decision with a way to get it wrong:

- The **path** is the declared path without its leading `/`, then `/<subdir>` when the joined subdirectory is not empty. The leading `/` has to go because an Open's path is relative to the connection it is written on (`Server::open()` refuses `/export/docs` as `BadPath`, and the first mutation below shows every directory use failing that way). The subdir is joined by the root and not split off, so that each server in the chain consumes only what it knows: the provider's server walks `export` and `docs` (the declaration) and then `sub` (the subdirectory the hops joined) in one walk, and a part never has to be passed to the next server.
- The **flags** are the use's rights as the `Perm*` bits, and `ProtocolDirectory`. A right is the same bit in a connection's `Operations` and in an Open's `Flags`, so the conversion is a cast, pinned by two `static_assert`s against `PERM_READABLE` and the mask of known permissions. The provider's server makes the connection with exactly these rights, and every later Open through it is checked against them, so rights only shrink from the entry on. `ProtocolDirectory` is what makes a route whose path ends at a file fail: without it the server sees an Open that accepts any node kind and opens the file for the entry, so a consumer asked for a directory and silently got a file. With it the server answers `NotDir`. Nothing before step 4 could observe this bit.
- The **object** is S, which the write consumes whatever it answers.

The states the provider can be in decide what the consumer sees, and `forward()` already had the rule for a protocol: an exited provider, or one whose outgoing client end the Loop has closed, is `NotFound` (D23), logged in the shape of a routing failure with the provider as the place the walk stopped. A directory is the same rule, so the rule became a function both call; copying it would have left two places to change when a third state appears. The same holds for `write_open()`: encode, write, and say `cannot forward <name> to <provider>` when the channel is full or gone. For a directory `<name>` is the directory's name at the provider.

A provider that has not started yet needs nothing: `resolve()` made its outgoing pair when the route walked through it, so the write queues on the server end that the provider takes at its start. That is why `files` can come after `routing_checks` in `root.cm`.

Two checks belong to the root and not to the decoder or `route()`: a declared path that is empty, is only `/`, or does not start with `/` is `InvArgs` on S, with a line naming the consumer's use, the provider's declaration and the path (shown through `printable_path()`, since it is manifest bytes). The compiler never writes one, and the decoder is structural by design, so this is the root's refusal as `serve_svc()`'s is for a protocol use that is not under `/svc`. A joined path longer than the buffer is the same, though `route()` has checked it: the buffer is fixed and the length is cheap to check again.

**Change**
```cpp
// before, Realm::run()
} else if (routed->provider != nullptr || routed->rights.is_none()) {
    // Under D3 a directory comes only from the built-in `boot` ...
    served = Status::NotSupported;
} else {
    served = m_boot->serve(server, routed->subdir.view(), to_operations(*routed->rights));
}

// after
} else if (routed->rights.is_none()) {
    served = Status::NotSupported;           // a directory use states rights; the decoder does not check
} else if (routed->provider != nullptr) {
    forward_directory(instance, use, *routed, server);   // consumes |server|, whatever the provider's state
} else {
    served = m_boot->serve(server, routed->subdir.view(), to_operations(*routed->rights));
}
```

```cpp
// forward_directory(), the part that builds the Open
auto const declared = routed.path.substr(1);          // export/docs
...                                                   // + "/" + subdir when there is one
write_open(provider, routed.name,
           to_flags(*routed.rights) | ours_io::Flags::ProtocolDirectory,
           {path_bytes.data(), length}, server);
```

Rejected: keeping `forward()` whole and writing `forward_directory()` beside it as a copy, which is shorter to write and leaves the exited-provider rule in two places; and a state per directory entry in the root (a Loop slot on the entry's first message, Fuchsia's way), which is decision 5 (b) of the design, kept as the answer if the 64-message bound of a provider's outgoing queue or the window below ever matters.

**Reference**
The design's `.agents/tasks/directory-capability/DESIGN.md`, "The Open on its way" and "One Open crossing mount points", decision 5 (a). The path join is the one the design cites for Fuchsia: `DirConnectorOutgoingRouter` joins `subdir` to `source_path` (`src/sys/component_manager/src/model/component/instance.rs:1682`). Fuchsia connects a provider on the entry's first message (`serve_processargs/src/namespace.rs:145-175`); ours does it when the consumer starts (D8 a), which is the departure the maintainer accepted.

**Verification**
- Build, `boot_check.py --out out/dc-step4` on both images: dbg PASS, 44 passed, 4 skipped, 0 failed; rel PASS, 41 passed, 8 skipped, 0 failed, 6 of 6 required; host tests 151 passed. The same counts as before the change. The cases are in `routing_checks`, which prints `routing_checks: passed` only on rel, so what is new is that line meaning more: 26 more cases.
- The rel log shows the whole life of the new route: `init: started files` after `init: started echo`, and, from the provider's console, `ioservice: the outgoing directory refused an Open of "export/ghost": NotFound` and `... of "export/docs/hello.txt": NotDir`, the two Opens the root forwarded that the provider's server refuses.
- The root has no host test (`system/service/init/test/` holds `autorun_test.cpp`), so `forward_directory()` is checked on the target, by mutation. Each mutation was applied to the source, the rel image built and booted, and the source restored; all are caught by `routing_checks`, whose own mutations are in `routing-checks.md`. The counts are `boot_check.py`'s FAILED lines, the closing `n check(s) FAILED` line among them:
  - the declared path's leading `/` kept: 19, `BadPath` on every directory;
  - the joined subdir dropped: 9, `/files-sub` lists the whole tree;
  - the subdir joined with another separator than `/`: 5, `NotFound` for `/files-sub`;
  - `ProtocolDirectory` dropped: 3, and only the two cases of a path that ends at a file notice it (a directory behaves the same with and without the bit);
  - the rights dropped: 28, `AccessDenied`; every right instead of the use's, and the use's plus `ModifyDirectory`: 2 each, the one case that asks `/files` for `MODIFY_DIRECTORY`, since a read-only `VmoFile` refuses write and execute whatever the connection holds;
  - the old branch put back (`NotSupported`): 19.
- The `InvArgs` path is reached by no manifest the compiler accepts, so it was checked by inverting its condition: every directory route to a provider then logs `init: routing_checks's use of "files" cannot be forwarded: files's declaration of "docs" states the path "/export/docs", which is not absolute below /` and closes the entry with `InvArgs` (19). The condition was restored.
- clang-tidy (`prebuilt/clang/llvm-23`, `-p out/dc-step4`) on `realm.cpp`: no diagnostic. `format_cpp.py --check` clean.
- Not measured: the exited-provider row and the failed-to-start row of the design's table. Nothing in the dev image makes a provider exit (that is step 5's `stop` node) or fail to start, and the window of the fifth row cannot be made on purpose. Both go through `provider_serves()`, which `forward()` has used for a protocol since 2026-10-30 and `routing_checks` exercises for echo.

**Deliberately not done**
- A state per entry, a timeout, or a retry in the root. The write is the whole of the root's part.
- A change to `log_route_error()`. A directory denied at the declaration reads `routes nowhere: AccessDenied at files (declaration of "docs")`, which is true and does not say the provider allows less than the use asked for; step 1 noted it. The warning of the build says it in full.
- A host test for the root. It would need the Loop, the fake kernel and a Realm; the target run covers each branch that can be reached.
- `Realm::connect()` is untouched: a directory is routed once, at the consumer's start, and never per Open.

**How it was found**
None. A new feature, from the design. The one thing the design had not seen is in the entry for `routing_checks` (the first Open on an entry that the provider later refuses reads `PeerClosed`).

**The user's prediction**
None.

**Three questions you should be able to answer**
1. The root writes the Open and keeps nothing. What would it have had to keep if it had answered the consumer itself, and why would that make a filesystem that waits on a disk unusable?
2. Drop `ProtocolDirectory` from the flags and the five uses that work still work. Which route would notice, what does the provider answer with the bit, and what did the consumer get without it?
3. A declared path is `/export/docs` and the joined subdir is `sub`. Which server consumes which part of `export/docs/sub`, and why did the root join them instead of sending two Opens?
