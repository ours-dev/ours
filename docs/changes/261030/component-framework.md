# component-framework changes

## Title: a component may declare and expose a directory (2026-10-30)

**Files**
- `system/component_manifest.py` — the three refusals lifted: `capabilities` accepts `{ directory = ... }`, `expose` accepts a directory from self or a child, `offer` accepts a directory from self or `#child`. A directory capability refuses `type`.
- `system/test_component_manifest.py` — the three refusal tests turned into acceptance tests, plus the no-type and undeclared-source refusals.

**Symptom**
No component could provide a directory: the compiler refused a declared directory ("a component cannot declare a directory"), an exposed one, and an offer of one from anything but the parent. The only directory a route could carry was the root's built-in `boot`.

**Cause**
A deliberate non-goal of the component-framework design: "Directory capabilities provided by components, and vfs remote nodes" are left out on purpose, because nothing provided one. The macro-filesystem task needs exactly that: a composing server whose served tree is a directory capability routed to its users. The remote-node half of that non-goal sentence was reversed first (ioservice forwarding, `docs/changes/261030/ioservice.md`); this reverses the directory half. The route walk, the rights rules and the compiled format were already written over kinds generically — only the permission was missing.

**Change**
```python
# before
if isinstance(clause, dict) and "directory" in clause:
    raise ManifestError(f"{at}: a component cannot declare a directory; ...")
# after
clause = _check_keys(clause, ("protocol", "directory", "type"), at)
kind, names = _capability_names(clause, at)
if kind == KIND_DIRECTORY and "type" in clause:
    raise ManifestError(f"{at}.type: a directory capability has no `type`; ...")
```
The same lifting in `_exposes` (and `directory` added to its allowed keys) and in `_offers`, whose `allowed = ("parent",)` clamp for directories is gone. Exposes still state no rights, offers still may narrow with `rights`/`subdir`, and a use still comes from the parent only — the hop rules are unchanged, so a directory route is compared by its rights exactly as the built-in `boot` route always was.

**Reference**
Fuchsia manifests carry `directory` capabilities declared, exposed and offered between components (`fuchsia.component.decl`); `fshost` exposes the filesystems' directories this way (`src/storage/fshost/src/export.rs`). Ours keeps the same shape with the subset's flat keys.

**Verification**
`python3 -m unittest system.test_component_manifest`: 102 tests pass, the three new acceptance tests among them. `system.test_component_builtins`: 26 pass. Not measured: nothing to measure.

**Deliberately not done**
- No runtime change in this commit: init still answers NotSupported for a directory route that ends at a provider, until the next entry.
- A directory `use` still comes from the parent only, as every use does (D3).

**How it was found**
None.

**The user's prediction**
None (delegated, 2026-10-30; see `docs/changes/261030/ioservice.md`).

**Three questions you should be able to answer**
- Why does a directory capability refuse `type`, and what are its routes compared by instead?
- Which of the hop rules changed for a directory from a component, and which stayed exactly as the built-in `boot`'s?
- Why was lifting three refusals enough for the compiler — what had already been written generically?

## Title: the root routes a directory use into a provider's outgoing directory (2026-10-30)

**Files**
- `system/service/init/realm.cpp` — `Realm::run`'s directory-use branch forwards to a provider instead of answering NotSupported; `Realm::forward` takes the path and the flags, so the same fire-and-forget write serves a protocol's `svc/<name>` and a directory's exposed name; a `to_flags` beside `to_operations`.
- `system/service/init/realm.hpp` — `forward`'s new shape and comment.
- `system/lib/component/include/component/route.hpp` — `Route::provider`'s comment says where a directory is served, as a protocol's already did.

**Symptom**
Latent until a manifest routes a directory to a provider (the compiler change above makes that possible): `Realm::run` answered NotSupported for every directory use whose route ended at a component, so a component's exposed directory could never reach a user's namespace.

**Cause**
The routing walk (`component::route`) was already generic over kinds and ended at the provider's declared name; init's directory branch predates any provider of a directory and clamped the two cases together (`routed->provider != nullptr || routed->rights.is_none()` → NotSupported).

**Change**
```cpp
// before
} else if (routed->provider != nullptr || routed->rights.is_none()) {
    served = Status::NotSupported;
}
// after
} else if (routed->provider != nullptr) {
    // join name + subdir, then:
    forward(*routed->provider, path, to_flags(to_operations(*routed->rights)), server);
}
```
The use's rights become the Open's flags (the PERM bits share Operations' values), and the entry's server end travels in that one Open — the same fire-and-forget shape a protocol's Open takes, so the component and the provider talk directly from then on. The provider serves the opened path in its outgoing directory: `svc/<name>` for a protocol, the exposed name for a directory.

**Reference**
Fuchsia `OutgoingConnector::send` for protocols and component_manager's directory routing, which opens the exposed directory in the provider's outgoing dir and hands the channel to the user's namespace (`src/sys/component_manager/src/model/capabilities.rs`).

**Verification**
`boot_check.py`: dbg PASS (44 passed, 0 failed), rel PASS (41 passed, 6/6 required), host-tests PASS 148. clang-format clean, clang-tidy quiet on the changed lines. The provider path itself first runs on target with the composing server (next change); not measured: forwarding latency.

**Deliberately not done**
- No watch on the provider's liveness beyond what protocols already have (an exited provider answers NotFound; a dead outgoing channel drops the Open and the client sees PEER_CLOSED).
- No caching of the route: the directory entry is routed once at start, protocols on every Open, as before.

**How it was found**
None.

**The user's prediction**
None (delegated, 2026-10-30).

**Three questions you should be able to answer**
- Why do the use's rights become the flags of the forwarded Open, and what invariant of route() guarantees that is exactly what the connection should carry?
- Why does the same `forward()` serve both kinds, and what two things differ between a protocol's and a directory's call?
- What does a component see in its namespace when the provider of its directory use has already exited?
