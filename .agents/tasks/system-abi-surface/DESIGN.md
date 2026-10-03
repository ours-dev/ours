# system-abi-surface: how //system exposes its public headers and the syscall interface, and what to fix

**Date**: 2026-10-02
**Status**: Proposed

## Problem
The user asked an expert panel whether the way `//system` exposes the system header directory and the syscall user interface has problems, and how to change it. The arrangement under review:

- `system/public/ours/*.hpp`: the ABI headers (`types.hpp`, `status.hpp`, `rights.hpp`, `signals.hpp`, `syscall.hpp`, `process_args.hpp`, `syscall/resource.hpp`) side by side with kernel support headers (`assert.hpp`, `panic.hpp`, `compiler.hpp`, `macro_abi.hpp`, `config/annotates.hpp`, `syscall/time.hpp`). One `:public` target and `public_headers.gni` list them; `:public.config` puts the directory on every include path, toolchain-wide.
- `system/public/ours/syscalls.gni`: the description of the 20 calls, compiled by `build/abi/gen_syscalls.py` into the private call numbers, the vDSO stubs, the kernel's typed declarations and register shims, and the `extern "C"` declarations `<ours/vdso.hpp>` includes. `libours.ifs` is derived from the same list in GN, and `vdso.abi_check` compares it with what the vDSO exports.
- `//system/lib/ou`: one hand-written named entry per call; ring 3 is meant to call nothing else (`layering.md`).

How the panel worked: one surveyor mapped the facts; five experts reviewed it from five sides (ABI soundness, Zircon parity, build and trust boundary, user-facing API, generator and kernel side); a skeptic per expert re-checked every claimed problem in the code; three architects proposed competing target designs; a judge was to merge them. The build lens's skeptic and the judge failed on network errors, so the build findings below are unverified (most of them restate verified findings from other lenses), and the merge in this document was done by the orchestrating session, which spot-checked the key facts (the GPL header, `RsrcKind`'s values, the raw calls in the shell, the internal status codes, `UserPtr`'s trait). 56 problems were raised and none was refuted; most are medium or low.

The verdict: the pattern is right and no settled decision needs reversing. One description compiled into both halves of the call, the vDSO as the only trap site, numbers private to the kernel and the vDSO, and a wrapper layer above the raw symbols are all Zircon's shape. What is wrong is enforcement and hygiene: the rules the design states are not held by anything, and several facts are written down two or three times and have drifted.

The problems that matter, most severe first (panel ids in brackets):

1. The ou-only rule has no mechanism, and the tree's main example breaks it [ZX-1, API-1, API-2, BUILD-01]. `<ours/vdso.hpp>` reaches every user program through `//system:user.headers`; `examples/shell/shell.cpp` calls `ours_*` directly at 18 sites. Part of the reason is that `ou` drops ABI parameters inconsistently: `ou::channel_create` passes its options, `ou::process_create`, `ou::thread_create`, `ou::channel_read` and `ou::channel_write` fix theirs to `None`, so the shell's option-bit and bad-pointer tests had no legal way to reach the call.
2. Kernel-internal status codes are in the public `Status`, and nothing stops them at the boundary [ABI-04, GEN-1, ZX-2]. `Delayed` (produced by the scheduler, `fair.cpp:246`), `InternalIrqKilled` and `InternalIrqRetry` (`status.hpp:22, 95-96`). The `blocking` attribute is validated and then dropped, so nothing retries `InternalIrqRetry` in the vDSO as design 4.5 says (Zircon: `zircon/kernel/lib/userabi/vdso/syscall-wrappers.cc:14-22`). Latent until a user thread blocked in a call can be suspended.
3. The ABI headers are not self-contained [ABI-01, ZX-9, BUILD-05, ABI-06, BUILD-12]. They are written in `u32`, `usize` and `_bit`, which a prelude under `//kernel/include` supplies by force-include in three configs; their bitmask operators come from ustl as global-namespace templates (`enum_bits.hpp:28`), which any `operator|` in a caller's namespace hides; and ustl in turn includes `system/public` with no GN edge.
4. The call contract is written by hand three times and has drifted [ZX-4, API-3, GEN-2, ZX-5, API-7, ZX-13, GEN-8]. `syscalls.gni`'s `vmo_create_child` still says only slices are built; `ou/vmo.hpp` says a snapshot keeps the parent's rights; `vmo_replace_as_executable` lacks `release = true`; the design skill's add-a-syscall recipe still names a hand-written stub and a hand-written declaration; `vdso.hpp` and `user_executable.gni` still describe `bind_imports()`.
5. The vDSO is mapped executable as a whole [GEN-4, ZX-3]: `.dynsym`, `.dynstr` and `.dynamic` included, in one R+X mapping (`userboot.cpp:528-531`), and the vDSO-only check is a range over that whole image. Zircon maps only the code segment executable. The per-call sysret check and the build-id pairing are recorded deferrals (design 4.7 item 7, D13).
6. A type crossing the boundary only has to be trivially copyable [ABI-05, GEN-5] (`user_copy.hpp:103`), so the first struct out-parameter with padding would hand kernel stack bytes to ring 3. Zircon's `is_copy_allowed` also requires standard layout and unique object representations. No struct crosses today; this is the cheapest moment.
7. `RsrcKind` breaks the ABI enum rules [ABI-07, ZX-7, BUILD-09]: implicit values, no fixed underlying type, a `Root` kind, `System = 4` where Zircon has 5. Its header is GPL-2.0 inside the MIT `system/public`, it is included by ring-3 programs, and it is missing from `public_headers.gni`.
8. Hygiene [ABI-08, ABI-09, ZX-8, BUILD-04, BUILD-06, GEN-3, GEN-9, GEN-10, ZX-14]: kernel-only headers in the public directory; `public_headers.gni` is incomplete and gates nothing; the generator and the ABI verifier have no tests and nothing pins their outputs; `libours.ifs` comes from a second parser of the description; the process-name limit is private to the kernel though callers must respect it.

## Goals and non-goals
Goals:
- Ring 3 reaches the kernel only through `ou`, and the build makes any other route a compile error.
- `ou` is generated from `syscalls.gni`, with typed handles, so that adding a call is a description entry plus a handler body, and a call's ownership rules are in the types.
- The ABI headers compile on their own, and the set of public headers is an explicit list the build checks.
- Every fact about a call is written once, in the description, or checked against it.
- Nothing kernel-internal leaks through the boundary: status codes, padding bytes, the vDSO's non-code segments.

Non-goals:
- A C-compatible ABI. The headers stay C++; a non-C++ includer gets an `#error` instead of an empty file, and the choice is recorded as a deviation.
- A sysroot or an out-of-tree SDK, linking programs against `libours.ifs`, and goldens of the generated outputs for a frozen ABI. They wait for the first binary built separately from the kernel, as the filesystem protocol already records for its own (`docs/design/260924/userspace-filesystem.md:271`).
- Replacing the GN-plus-Python description with FIDL (a recorded deviation).
- The per-call sysret check and build-id pairing beyond what step 10 needs (recorded deferrals).

## Design
### Target state
**ABI headers (`//system/public:public`).**
- Two explicit lists in `public_headers.gni`: `ours_abi_headers` (`int.hpp` new, `types.hpp`, `status.hpp`, `rights.hpp`, `signals.hpp`, `syscall.hpp`, `process_args.hpp`, `syscall/resource.hpp`) and `ours_support_headers` (`assert.hpp`, `compiler.hpp`, `panic.hpp`, `macro_abi.hpp`). A list-versus-directory check fails the build on any header in neither list. Kernel-only headers (`syscall/time.hpp`, `config/annotates.hpp`) move under `kernel/include`.
- Each ABI header compiles alone: `ours/int.hpp` holds the integer aliases now in `kernel/include/ours/prelude.hpp` (which includes it), bits are spelled `1U << N`, and the bitmask operators are emitted by one macro in the enum's own namespace, so argument-dependent lookup finds them (ustl's macro forwards to it; one implementation). A standalone-compile action proves it for the user and host toolchains.
- Every cross-ABI enum is `: u32` with explicit Zircon values; every struct that crosses has fixed-width fields, explicit padding and `static_assert`s on size and offsets, as `ProcArgs` does; every limit a caller must respect, the name limit included, is in `syscall.hpp`.
- `Status` is `enum class [[nodiscard]] Status : u32` (decided).

**The description and the generator.**
- `syscalls.gni` is the reviewed form of the ABI, and gains what a typed `ou` needs: a handle argument's object type (as Zircon's `Handle:VMO`), the option enum an options argument takes, and whether a handle argument is consumed (`release`, which exists today and is honoured nowhere).
- `gen_syscalls.py` is the one front end: it also renders `libours.ifs`, resolves every type name, checks that a `count` names an integer, and is covered by host unit tests and fixture goldens.
- Rules recorded in the description's header: a published signature is never retyped or reordered (a changed call gets a new name), as status values are already append-only.

**`ou`, generated and typed (decided).**
- The generator emits `ou`: one entry per call, every ABI parameter passed through (options included, defaulting to `None`), pointers forwarded unchanged, a `[[nodiscard]]` status.
- Handles are typed, in Zircon's `lib/zx` shape: a move-only owner per object type (`ou::Vmo`, `ou::Channel`, ...) closing on destruction; a consumed argument is taken by value (`ou::Vmo&&`), so ownership is in the signature; a raw value can be wrapped explicitly, which is how ABI conformance tests pass bad handle values through `ou`.
- The shape is piloted on the vmo family before it is generated for every call, and settled before the K23 launcher executor is written, since that is the first ring-3 code to chain `vmo_replace_as_executable`, `process_start` and multi-handle `channel_write`.

**The boundary in the build.**
- `<ours/vdso.hpp>` and the generated declarations are reachable only from `//system/lib/ou` (a private dependency, with an exact GN visibility list); anywhere else an `#include <ours/vdso.hpp>` stops on an `#error` that names `ou::`. `//system:user.headers` stops carrying the generated include directory.
- `:public.config` stops being toolchain-wide for ring 3: a program gets the ABI headers through a dependency, so a missing one is an error. The kernel toolchains keep it toolchain-wide as one trust domain, recorded as a deviation from Zircon's `visibility = [":*"]`.
- `gn check` runs over ring 3, the ABI and the syscall libraries.

**The kernel side.**
- `UserPtr` requires Zircon's `is_copy_allowed`: trivially copyable, standard layout, unique object representations.
- The vDSO is mapped by segment: only its code segment executable, and the vDSO-only check covers that segment.
- Kernel-internal status codes: open (decision below).

### Order of work
Each step is one commit, builds and boots both images, and is introduced to the user before it starts. Steps 1 to 5 are small and agreed by all three architects.

1. **Make the record true.** Docs and data only: the drifted text in `syscalls.gni` (`vmo_create_child`, `object_wait_one`'s deadline, `release` on `vmo_replace_as_executable`, the `release` definition for arrays, the stale `sys_*`), `status.hpp`'s misplaced comment, `ou`'s headers and README, the stale `bind_imports()` references, the deviation table (C++-only headers, the ou-only rule without exceptions), and the add-a-syscall recipe in `layering.md` (done with this design). **Done 2026-10-02:** an audit of every call family and of the texts about the ABI as a whole, each finding re-checked by a skeptic before it was applied, then the applied diff reviewed again; the record is `docs/changes/261002/abi.md`.
2. **`[[nodiscard]] Status`**, and the silent drops it surfaces, kernel and ring 3; counted first and split by module if large. **Done 2026-10-03:** 44 drops, all in the kernel, answered in four module commits, then the attribute; the record is `docs/changes/261003/abi.md`.
3. **ABI values.** `RsrcKind : u32` with Zircon's values (`MmIo 0, Irq 1, IoPort 2, System 5`, no `Root`), `NAME_MAX` published in `syscall.hpp`, `Clock` and the other kernel-only names out of `types.hpp` as far as they have no ring-3 user. **Done 2026-10-03, in part:** `RsrcKind` and `MAX_NAME_LEN` (not `NAME_MAX`, a `<limits.h>` macro) landed, and the seven names nothing used were deleted (`docs/changes/261003/abi.md`). The nine kernel-only names that have users (`PhysAddr`, `CpuNum`, `INVALID_CPU_NUM`, `NnId`, `HIrqNum`, `VIrqNum`, `PgOff`, `Ticks`, `Time`) stay in `types.hpp`: the user deferred where they go.
4. **`UserPtr` requires ABI-safe types.** **Done 2026-10-03:** the `CopyAllowed` concept (trivially copyable, standard layout, unique object representations), with its cases in the user_copy host test; the record is `docs/changes/261003/user-copy.md`.
5. **Everything through `ou`, and only `ou` can reach the raw calls.** `ou` exposes the option words it hides; the shell's 18 raw sites move to `ou`; then the GN visibility and the `#error` (two commits).
6. **Self-contained ABI headers.** `ours/int.hpp`, the bitmask macro in the enum's namespace, the ustl edge removed from the ABI closure, the standalone-compile check.
7. **The lists made real.** `public_headers.gni` split and checked; kernel-only headers moved; `:public.config` by dependency for ring 3; `gn check` on.
8. **The generator, tested and single-sourced.** Unit tests and fixture goldens; `libours.ifs` rendered by it; the description gains handle object types and option enum types.
9. **Generated, typed `ou`.** A short design of the typed layer first (introduced to the user), the vmo family as the pilot, then every call. Several commits; before the K23 launcher executor.
10. **The vDSO mapped by segment**, its check narrowed to the code segment.

Later, each with its own trigger: the internal status codes and the `blocking` retry (once decided); the per-call sysret check and build-id (design 4.7 item 7, D13); CFI in the vDSO stubs; a ring-3 libc++ configuration of its own before anyone wires the shared one to kernel primitives (BUILD-11); the sysroot and the link stub (first separately built binary).

## Alternatives considered
- **Minimal change** (7 small steps): fix only the verified defects, keep the hand-written `ou`, keep the headers relying on the prelude. Rejected as the whole plan: it leaves the headers unusable outside the tree and the call contract in three hand copies; its first steps are steps 1-5 here.
- **Zircon-faithful** (12 steps): everything here plus goldens of the real outputs and the precise sysret check now. Its typed `ou` was hand-written in `lib/zx` style; the user chose generation instead.
- **C++-first** (11 steps): close to this design (the bitmask macro, the kernel vocabulary moved out, generated typed `ou`); this design takes its header steps and keeps Zircon's ordering of the boundary work.
- **An explicit exception list for ABI conformance tests** (the shell calling `ours_*` under a listed permission, as Zircon's core-tests call `zx_*`). Rejected by the user: everything goes through `ou`, which therefore must pass every ABI parameter through unchanged.
- **Keeping `ou` hand-written with a completeness check driven from `syscalls.gni`.** The panel's recommendation; rejected by the user, for whom generating `ou` has always been the plan.
- **A C-compatible ABI layer.** Not wanted: no C consumer exists or is planned, and the project has no legacy constraints. Recorded as a deviation instead.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| How the ou-only rule holds for ABI conformance tests | everything through `ou`, which passes every ABI parameter; an explicit exception list for conformance tests | everything through `ou` | None | Everything through `ou`, 2026-10-02 |
| Typed handles in `ou` | bare `HandleValue`; `lib/zx`-style move-only owners, consumed arguments taken by value | typed, piloted on the vmo family, settled before K23 | None | Typed, 2026-10-02 |
| Who writes `ou` | hand-written plus a completeness check; generated from `syscalls.gni` | completeness check (the panel) | None | Generated, 2026-10-02 (the user: a generator has always been the plan) |
| `[[nodiscard]]` on `Status` | on the type; per `ou` entry; none | on the type | None | On the type, 2026-10-02 |
| The K23 executor before or after the typed `ou` | write it now against today's `ou` and migrate it with step 9; pilot the typed `ou` first and write it after | now, and migrate | None | Now, 2026-10-02: the executor is one more caller step 9 migrates |
| Kernel-internal status codes | a kernel-private enum with a boundary guard (touches `fair.cpp`, which is hands-off); keep them public, have the vDSO retry `InternalIrqRetry` and assert the others never cross (Zircon keeps `ZX_ERR_INTERNAL_INTR_RETRY` public and consumes it in the vDSO); leave as is | keep public, vDSO retry plus a boundary assertion | None | |
| License of `system/public/ours/syscall/resource.hpp` (GPL-2.0 in an MIT directory, included by ring 3) | relicense to MIT; keep and move out of `system/public` | relicense (the copyright holder's call) | None | |
| C++-only ABI headers | keep, with `#error` for other languages and a deviation row; add a C layer | keep and record | None | |

## Testing and verification
- `ours-verify` on both images at every step; the shell's checks keep passing after they move to `ou` (step 5), and the snapshot, user-copy and seal commands keep passing on dbg.
- Host tests: the generator's unit tests and fixture goldens (step 8); a standalone compile of every ABI header in the user and host toolchains (step 6); the list-versus-directory check (step 7).
- The boundary: after step 5, a probe target that includes `<ours/vdso.hpp>` outside `ou` must fail to build; it is run once by hand and recorded, since a build that is meant to fail cannot sit in the default graph.
- Not verifiable yet: compatibility with a separately built binary (none exists), and the `blocking` retry (nothing can suspend a thread inside a call).

## Open questions
- The kernel-internal status codes (decision row above).
- The license of `syscall/resource.hpp` (decision row above).
- The exact shape of the typed `ou` layer: whether the generator emits classes with member functions (`vmo.create_child(...)`, as `lib/zx`) or free functions over typed handles; how a multi-handle array (`channel_write`) is typed; and how a conformance test wraps a raw bad value. Step 9 introduces it before any code.
- Where the kernel-only headers moved out of `system/public` land (`kernel/include/ours/...` is the default), and whether `assert.hpp`/`panic.hpp` stay public with a ring-3 panic provider, as Zircon's `assert.h` does with `__zx_panic`.
- The build lens's findings were not adversarially checked (network failure); each is re-checked when its step starts.
