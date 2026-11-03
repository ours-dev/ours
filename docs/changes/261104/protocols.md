# protocols changes

## Title: the six protocol libraries become one, in namespace prot (2026-11-04)

**Files**
- `system/lib/protocols/**` — the new library: `include/prot/{block,io,resource,acpi,fshost,process}.hpp` (the codecs), `include/prot/{block,resource,acpi}_client.hpp` (the user-only client plumbing), `src/*.cpp` (the codecs' out-of-line bodies), `test/` (the six host test suites), one BUILD.gn with the headers/client/source_set/tests shape each old library had.
- `system/lib/ours.{block,io,resource,acpi,fshost,process}/` — deleted.
- 67 client files tree-wide — includes (`<ours_X/...>` → `<prot/...>`) and namespace tokens (`ours_X::` → `prot::<family>::`).
- 19 BUILD.gn files — deps collapsed onto `//system/lib/protocols` (codec), `:client` where the target calls client plumbing (shell, the three drivers, btreefs, acpi-route, routing_checks), and the root's six tests groups folded into one.

**Symptom**
None — a refactor, not a fix. What it changes is what the layering says:
six tiny libraries with `ours_<family>` namespaces blurred where the kernel
ABI ends, because `ours::` is the vDSO's namespace — the syscalls and the
types the syscalls speak (`ours::Status`, `ours::Uandle`, `ours::Rights`).
A protocol is a contract between components; it speaks the kernel's types
and lives outside the kernel's namespace.

**Cause**
None — the structure followed the libraries as they grew, not a rule. The
rule now written down (and enforced by the namespace): **only kernel-level
things carry `ours::`**; OS-layer contracts go in `prot`.

**Change**
The codec bytes are identical; the wire names stay the platform's family
prefix ("ours.block.Block", the role "fuchsia.*" has in Fuchsia — the C++
binding namespace does not determine it). Every dependant target now deps
`//system/lib/targets:client` where it calls client plumbing, bare
`//system/lib/protocols` where it only speaks the codec.

```cpp
// before: six namespaces, six libraries, six deps on most targets
#include <ours_block/ours_block.hpp>
namespace ours_block { ... }
// after: one library, one include root, one namespace with sub-namespaces
#include <prot/block.hpp>
namespace prot::block { ... }
```

**Reference**
- The namespace rule is the layering reference's own (`kernel-design`,
  references/layering.md): ring-3 calls the vDSO as `ours::<name>()` —
  `ours::` is the kernel's user-facing surface, and nothing else claims it.
- The wire-name rule is Fuchsia's: platform protocols are `fuchsia.*` on the
  wire regardless of the C++ binding namespace.

**Verification**
Full `boot_check.py`: dbg 46 / rel 44 / host 161, diskless; dual-disk
(`DISK=` + `NVME_DISK=`) rel PASS with both disk chains silent-passing
(`nvme: serving ours.block.Block over 0x20000 blocks`, virtio-blk serving,
`routing_checks: passed`). clang-format and format_gn clean on everything
changed. Not measured: performance (no behavior change to measure).

**Deliberately not done**
- `ours.gpt`/`ours.btreefs` unmoved: disk and filesystem *formats*, not
  component contracts; their `ours_*` namespaces are a later mechanical
  rename if wanted.
- The pre-existing mixed abstraction in routing_checks and btreefs (raw wire
  frames beside typed clients) — predates this round, not touched.

**How it was found**
None — a refactor, not a bug fix. The move's own incidents are recorded in
RESULT.md ("Found along the way").

**The user's prediction**
None.

**Three questions you should be able to answer**
- A protocol speaks `ours::Status` but lives in `prot::block` — why is that
  not a violation of "only kernel things carry ours::"?
- A target deps bare `//system/lib/protocols` versus `:client` — what does
  each get, and why is the client target user-toolchain only?
- The wire name stays "ours.block.Block" while the C++ namespace is
  prot::block — which Fuchsia convention does this follow?
