# protocol-identity: a capability declaration carries the identity of its protocol, so that a mismatched route is stopped at the earliest point and says so

**Date**: 2026-10-29
**Status**: Proposed

## Problem
Routing matches a `use` to an `offer` by kind and name only (`detail::find_offer()`, `system/lib/component/src/route.cpp`). Nothing says what protocol a name stands for, so a route that ends at the wrong protocol is a valid route.

The experiment of 2026-10-29 shows what that costs. `root.cm` offered the built-in `ours.process.Launcher` to `#pc-ps2` under the name `ours.resource.Irq`, which is what pc-ps2's manifest uses. The result:

```text
init: started pc-ps2
pc-ps2: getting the IRQ resource: PeerClosed
init: pc-ps2 exited with 1
```

The route succeeded, so the root said nothing. pc-ps2 sent an ours.resource request to the Launcher, which did not know the method and closed the channel without an epitaph, as the protocols lay down. pc-ps2 reads PeerClosed, which looks like a server that died. No line anywhere says that two protocols were mixed up, or which offer did it.

Fuchsia has the same gap by design: `cm_rust::UseProtocolDecl` has a `source_name` and no type, and `ProtocolDecl` a `name` and no type (`src/sys/lib/cm_rust/src/use.rs:66`, `capability.rs:54`), so a protocol is its name. ours made the gap wider by letting an `offer` rename a capability with `as`: after a rename the name no longer says what the protocol is. The user asked on 2026-10-29 to fix it, to choose the point at which a mismatch is stopped with care, and to make the error output say what is wrong.

## Goals and non-goals
Goals:

- A route that resolves to a capability of another protocol than the use asked for is an error, found as early as the information exists: when the image is built, and again when the root routes the Open.
- The error says which use, which protocol it wanted, which protocol the route ended at, and the offer or expose on the way, to the person building, to the person reading the root's log, and to the component that asked.
- A manifest that does not rename anything keeps working unchanged.

Non-goals:

- The layout of a protocol's messages and its versions. Identity is one string per protocol; two protocols that share a name and differ in layout are not told apart.
- Directory capabilities. Their rights are already compared at every hop, and the only directory is the built-in `boot`.
- A generator for protocol codecs, or a type derived from a codec library (S6 in `docs/design/261004/component-framework.md`).
- Making the deliberate failures of routing_checks fail the build. `ours.test.Unoffered` and `denied` are routes that must not resolve; the build check looks only at routes that do.

## Design
**Objects.**

- **Protocol type.** A string naming the protocol a capability speaks, such as `ours.resource.Irq`. It is declared in three places: on a `capabilities` entry of a provider, on a `use` of a consumer, and on each protocol of the root's built-ins. An `offer` and an `expose` pass a capability on and carry no type: an `as` renames the capability, not the protocol.
- **Default.** An omitted type is the entry's own name. So nothing changes for a manifest that never renames, and the one place that does need a type is a `use` whose name differs from the provider's: routing_checks' `ours.test.Echo` becomes `{ protocol = "ours.test.Echo", type = "ours.examples.echo.Echo" }`. A `type` goes with a single protocol, not a list.
- **The rule.** When a walk ends at a source, the source's type must equal the use's type.

**Format.** `format::Capability` and `format::Use` gain a `String type` each; `VERSION` goes from 1 to 2; the decoder bounds-checks the new strings as it does the others and refuses version 1. The format is internal and its writer and reader are built together, so there is no compatibility to keep. `component::Capability` and `component::Use` expose `type()`, which returns the name when the stored type is empty.

**Interception, in the order a mismatch can be met.**

| When | Where | Catches | Does not catch | What the failure looks like |
|---|---|---|---|---|
| Build | the compile of a manifest that has children (`component_manifest.py`), over its whole tree | every route that resolves and ends at another type, in every image, before anything boots | a route that does not resolve (reported at run time, as now); an image not built from these manifests | the build fails; the message below |
| First Open | `component::route()`, called from `Realm::connect()` for every Open of a `/svc` node | the same, for a manifest that was not checked at build time (a hand-made image, a host test) | anything before the first Open of that name | the root's log line, and the component's channel closed with an epitaph |
| First message | the server | a different protocol, by an unknown method | a protocol with the same method names and a different layout | PeerClosed, as in the experiment |

The first two are new; the third is today's, kept as the last line of defence. The build check reuses the rules of `route()` (up through offers, down through exposes, to a capability or a built-in) so that the two cannot disagree; it differs in what it does with a route that does not resolve, which it leaves alone.

The build check needs the declarations of the whole tree and the types of the built-ins. The first comes from the child manifests: the template already collects each child's metadata for `--children`, and adds the path of its `.source.json`. The second is decision 3.

**Error output, one message per audience.**

1. The person building. The script prints the route and the fix, and exits non-zero, so that ninja shows it:

```text
error: //kernel/products/image/dev:root.cm: #pc-ps2 uses protocol "ours.resource.Irq" (type ours.resource.Irq),
       but the route ends at the root's built-in "ours.process.Launcher" (type ours.process.Launcher):
         use    #pc-ps2 "ours.resource.Irq"
         offer  root.cm "ours.process.Launcher" from parent to #pc-ps2 as "ours.resource.Irq"
       Offer a capability of type "ours.resource.Irq" under that name, or give the use the type it means.
```

2. The person reading the root's log. One line, in the style of the existing `log_route_error()`, naming both types and the declaration the route ended at:

```text
init: pc-ps2's use of "ours.resource.Irq" reaches another protocol: it wants ours.resource.Irq, the route ends at ours.process.Launcher (the built-ins)
```

3. The component that asked. Its channel is closed with the epitaph `BadType`, delivered the way `NotFound` and `AccessDenied` are today, so that pc-ps2's own report reads `getting the IRQ resource: BadType` where it read `PeerClosed`.

`RouteError` gains the two types as views (`want`, `got`), set only for this error; its `status` is `BadType` and its `verb` `Declare`, the declaration the walk reached, so no new verb is needed.

**Counterpart.** None in Fuchsia, which has no type; the nearest is its build-time route validation, `scrutiny`, which the component-framework design leaves as a later step (non-goals, `component-framework.md:53`). This design takes that step for the one property it needs.

## Alternatives considered
- **Keep the name as the identity.** Fuchsia's choice, and the status quo. Rejected: `as` makes the name differ from the protocol on purpose, and the experiment shows the cost.
- **A hash of the protocol's method set, or the ordinal of its first method, as the identity.** Rejected for now: it ties a manifest to a codec library the manifest compiler does not read, and the codecs are hand-written until the generator exists. A string can later be replaced by a generated one without changing the rule.
- **A check at run time only.** Rejected as the only check: it stops the mismatch at the first Open, when the system is up, where the build check stops it before an image exists.
- **A check when a component starts, for all its protocol uses.** Rejected: protocol uses are routed on every Open on purpose (D11), a second route walk at start would have to be kept equal to the first, and the build check is earlier anyway.
- **An epitaph for an unknown method in every server.** Rejected here: it reverses the rule written in the protocols (an unknown call closes the channel), changes every server that follows it, and only improves the message of a mismatch that is already stopped by the checks above. It may be worth doing for its own sake, as a separate item.
- **A new `Status` for a protocol mismatch.** Rejected for the first version: `BadType` already means a thing of the wrong type, and a new status changes the system call ABI (decision 4).

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| 1. What carries a protocol's identity | (a) a string `type`, defaulting to the name; (b) a hash of the method set; (c) the name alone, as Fuchsia | (a) | Not asked | |
| 2. When a mismatch is stopped | (a) at build time and at the first Open; (b) at the first Open only | (a) | Not asked | |
| 3. Where the build check gets the built-ins' types | (a) one data file read by the script and by `realm.cpp`, which also removes the second list in `BUILTINS`; (b) a list in `root.cm`'s GN, kept equal to `realm.cpp` by hand; (c) skip routes that end at a built-in | (a) | Not asked | |
| 4. What the component is told | (a) epitaph `BadType`; (b) a new `Status` | (a) | Not asked | |
| 5. An omitted `type` | (a) the entry's own name, strict; (b) unchecked, lenient | (a) | Not asked | |

Decision 1 departs from Fuchsia, and 3 (a) from the way `BUILTINS` is written today; both are the user's to make. 3 (c) leaves the experiment's own case, a route to the Launcher, unchecked at build time.

## Testing and verification
- **Host tests** for the `component` library: a use and a source of the same type route; a mismatch answers `BadType` with `want` and `got` set; the default is the name; a renamed use with an explicit type routes; a built-in mismatch; version-1 files are refused; the new strings are bounds-checked.
- **The script's own tests** (`system/test_component_manifest.py`): the same cases through the compiler, and the exact text of the build error for a mismatched fixture; a tree with a route that does not resolve still builds.
- **The experiment of 2026-10-29 as a test.** A fixture tree that offers the Launcher as `ours.resource.Irq` must fail the build with the message above. It cannot be an on-target check once the build check is on, since the image is never built; the host test of `route()` stands for the run-time half.
- **`ours-verify`**: both images boot, `routing_checks: passed`, and routing_checks' renamed use of Echo still works with its explicit type.
- **Not verifiable yet:** that a real component reads `BadType` from its channel. `ours_resource::get()` and `fsio` must be read to see whether the epitaph reaches the caller as it does for `NotFound`; if not, that is a finding, not a failure of this design.

## Open questions
- Does `ours_resource::get()` return the epitaph's status, or `PeerClosed`? The answer decides whether the third message reaches pc-ps2 as written.
- Manifests not reachable from `root.cm` are not covered by the build check. Is any such manifest built?
- Should the data file of decision 3 (a) also give the built-ins' kinds and rights, so that `BUILTINS` is generated whole? That would close item 1 of `docs/repairs/261028/init.md` as well, and is a larger change than this design needs.
