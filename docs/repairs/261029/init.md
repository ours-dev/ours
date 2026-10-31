# init outstanding repairs

## 2. `BootServer::add_service` names a `Connector` that no longer exists

**Severity**
Low: a stale comment.

**Symptom**
`system/service/init/boot_server.hpp` (the comment on `add_service`) says that an Open of the service "reaches the Connector with |service|". There is no `Connector` in `system/service/init`. An Open of a `/svc` service node reaches `Realm::connect()`, because `Realm` is the `LoopServer::Delegate` that `BootServer::set_delegate` is given. Found while revising `docs/guide/04-tree-servers.md`.

**Cause**
The class that routed a `/svc` Open was renamed or folded into `Realm`, and the comment kept the old name.

**Change**
Write "reaches the delegate's `connect()` with |service|" in the comment, which is how `set_delegate` already words it. No code changes.

**Reference**
None, this project's own design.

**Acceptance**
`grep -n "Connector" system/service/init/boot_server.hpp` finds nothing, and the build is unchanged.
