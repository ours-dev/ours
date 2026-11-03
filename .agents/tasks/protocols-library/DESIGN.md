# protocols-library: every wire protocol in one library, namespace prot

**Date**: 2026-11-04
**Status**: Proposed

## Problem

The wire protocols live in six small libraries named for the protocol family
(`ours.block`, `ours.io`, `ors.resource`, `ours.acpi`, `ours.fshost`,
`ours.process`), each with its own `ours_<family>` C++ namespace. But `ours::`
is the kernel ABI's namespace — the vDSO entries (`ours::<syscall>()` from
`<ours/vdso.hpp>`), `ours::Status`, `ours::Rights` — and the user's rule is
that only kernel-level things carry it: a protocol is an OS-layer contract
between components, not a kernel object. The six namespaces blur that
boundary, and six tiny libraries mean every component deps several of them.

## Goals and non-goals

Goals:

- One library `//system/lib/protocols`, one include root `<prot/...>`, one
  C++ namespace `prot` with a sub-namespace per protocol family:
  `prot::block`, `prot::io`, `prot::resource`, `prot::acpi`, `prot::fshost`,
  `prot::process` — the sub-namespace is the wire name's family word,
  mechanically.
- The same headers/client/codec split the six libraries have (headers build
  on the host toolchain where the codec tests run; client headers that make
  syscalls build user-only).

Non-goals:

- No behavior change: the codecs' bytes are identical, the wire protocol
  names ("ours.block.Block" and friends) unchanged.
- No kernel changes.
- `ours.gpt` and `ours.btreefs` stay where they are: they are disk and
  filesystem *formats*, not component contracts, and do not belong in a
  protocols library. Their `ours_*` namespaces are a separate question.

## Design

The new library keeps the shape the six have today, consolidated:

- `include/prot/block.hpp` … `process.hpp` — the codecs (namespace
  `prot::<family>`, include guard `PROT_<FAMILY>_HPP`).
- `include/prot/block_client.hpp`, `resource_client.hpp`, `acpi_client.hpp` —
  the client plumbing (block, resource, acpi have them today; they make
  syscalls and dep fsio, so their target is user-toolchain only).
- `src/block.cpp`, `src/io.cpp` — the codecs' out-of-line bodies (the two
  source_sets today).
- One `cpp_interface("headers")` over all six codec headers (deps:
  wire:headers, ustl:headers, system/public — unchanged per-family), one
  `cpp_interface("client")` over the three client headers, one
  `source_set("protocols")` over the two sources, one `group("tests")`.

The abstraction rule the move enforces (checked per header as it lands):
a protocol header carries wire encoding, request/reply structs, protocol
constants (names, limits) and client helpers — nothing else. No device types
(NVMe/virtio), no driver-framework types (ddk, loop, ioservice), no kernel
internals beyond the `ours::` ABI types the wire messages already speak
(Status, Uandle, wire). Findings are recorded in RESULT.md.

## Alternatives considered

- **One flat `prot::` namespace** — no per-family qualification. Rejected:
  the families share generic names (Flags, Geometry, Operations) that would
  collide; the sub-namespace is also the reader's map from C++ to wire name.
- **Rename the wire names to match** ("prot.block.Block") — rejected: the
  wire name is the platform's service-name family prefix, the same role
  "fuchsia.*" has in Fuchsia, and the C++ binding namespace does not
  determine it there either. Renaming churns every manifest and routing
  test for no behavioral gain.
- **Move `ours.gpt`/`ours.btreefs` too** — rejected: not protocols.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| C++ namespace | `prot::<family>` sub-namespaces / one flat `prot::` | sub-namespaces (shared generic names would collide flat) | — | user approved 2026-11-04 |
| Wire names | keep "ours.block.Block" / rename to "prot.*" | keep (the platform's service-name family prefix, as "fuchsia.*") | — | user approved 2026-11-04 |
| gpt/btreefs | leave for a later round / rename now | leave | — | user approved 2026-11-04 |
| Abstraction bar | audit each header on the way in | audit, findings to RESULT.md | — | user asked for it 2026-11-04 |

## Testing and verification

Host codec tests green (moved, renamed); full `boot_check.py` (dbg + rel +
host) green diskless and dual-disk; clang-format clean on everything
changed.

## Open questions

None — the shape is settled; the only implementation-time question is the
abstraction audit's findings, which land in RESULT.md.
