---
name: modern-cpp-development
description: Style rules and this repo's actual C++20/23 kernel conventions — object orientation (encapsulation, one owner per invariant, composition not inheritance), error handling (ustl::Result/ours::Status), ownership (Rc/Arc/Weak, no unique_ptr), locking annotations, namespacing, and build-time polymorphism
---

# Modern C++ Development

## Overview

Build features in modern C++ (20 and later, this repo pins C++23 via
`ours_cxx_version` in `build/config/cxx_version.gni`), following this repo's
actual conventions — a freestanding, no-exceptions, no-RTTI microkernel, not
generic application C++. Where a rule below cites a repo file, that file is
the ground truth if the two ever disagree; update this skill, don't silently
diverge from it.

## When to use

Activate (satisfy any of the following criteria):

- if the standard of C++ of this project is specified to `modern c++`.
- if the user explicitly requires you to code in `modern c++`.
- any time you create or edit a `.hpp`/`.h`/`.cpp`/`.cc`/`.cxx` file in this
  repo — see also `.agents/skills/cpp-write-and-verify`, which is the
  enforcement half of this skill (running the real `clang-format`/`clang-tidy`
  before a change is done).

## Requirements

### General style

1. Naming first. When a change could improve naming, types, or interfaces, do the naming. A wrong name misleads every future reader and costs nothing to correct, whereas an awkward interface at least describes itself honestly. Treat a stale or inaccurate name as a defect rather than a cosmetic issue: a `ClockSource *` called `CLKEVT`, an `m_priority` that is really a rating, a misspelled `entroll`. Naming corrections belong early in a multi-step repair, not in a tidy-up pass at the end that gets dropped for being cosmetic.

1. Const-first principle: In all contexts, every definition of a variable, function, function parameter, or template parameter should be qualified with one of __const__, __constexpr__, __constinit__, or __consteval__, unless the object needs to be mutable. Each of the three has one job and they are not interchangeable — see "Constants and configuration" below for which to reach for.

1. Constants and configuration — `constexpr` defines constants, a macro only carries a build-time value inward, and `const` merely marks something read-only:

    - **`constexpr` defines every named constant.** Never a `#define`. A macro constant has no type, no scope and no namespace: it takes part in whatever arithmetic it lands in without a word of complaint, and it claims its name in every translation unit that transitively includes the header. `HZ` was the cautionary case — a bare two-letter `#define` written *above* the includes in `kernel/include/ours/time/cfg.hpp`, so every header pulled in afterwards saw it occupying the global namespace. Typed as `constexpr auto HZ = static_cast<Duration::rep>(OURS_CONFIG_HZ)` in `ours::time`, it also stopped being able to change the deduced type of the duration computed from it (`Nanoseconds / u64` deduces the common type of `int64_t` and `uint64_t`, which is `uint64_t`).
    - **Macros carry build configuration in, and nothing else.** A value the build system must set from the command line — `OURS_CONFIG_HZ`, `MP_MAX_CPUS`, `KERNEL_DEBUG_LEVEL` — has to be a macro, because there is no other channel into the translation unit. Consume it exactly once, at the point of definition, into a `constexpr` of the right type; the rest of the code sees only that.
    - **`const` expresses read-only, not compile-time.** It promises this object will not be written through here. Reach for `constexpr` when the value is known at compile time and `const` when it is not.
    - **Never spell `constexpr` and `const` on the same declaration.** `constexpr static auto const PIT_MODE = 0x43;` is legal — the `const` is merely redundant there — but the two keywords carry different meanings in this codebase, and putting both on one object says the author could not decide which one they meant. `constexpr` already implies const, so write `constexpr static auto PIT_MODE = 0x43;`. This is about the declared object only: a `const` that belongs to a pointee (`static constexpr char const *SECTION`), to a parameter (`constexpr auto align_up(usize const value)`), or to a member function's implicit object (`constexpr auto when() const -> TimePoint<>`) qualifies a different entity and stays.

1. Encapsulate implementation details in a private nested type rather than leaking storage/bookkeeping into the public interface. Prefer a private nested `struct`/`class` (or a `detail`/`priv` sub-namespace for free functions — see Namespacing below) over exposing raw fields or helper functions that only the class itself should call.

1. Composition over inheritance, and prefer it over templated indirection too when the concrete type is already fixed at build time. `VmAspace` composes a plain, non-templated `ArchVmAspace m_arch;` member (`kernel/mm/include/ours/mm/vm_aspace.hpp:105`) — see "Build-time polymorphism" below for why this isn't CRTP and doesn't need to be. Composition is the default you may take without asking; **introducing a base class requires asking the developer first** — see "Object orientation" below.

1. Use alias declarations (`using X = Y;`) over `typedef`. When the alias is part of a container/allocator-like type's public API, use ustl's own vocabulary — `Element`, `Size`, `Iter`/`IterMut`, `Ref`/`RefMut`, `Ptr`/`PtrMut` — not legacy STL names (`value_type`, `iterator`, `reference`, `pointer`). This is enforced by `.clang-tidy` itself (its naming-section header comment: "USTL collection API aliases ... Do not introduce legacy STL aliases"). The one sanctioned exception is an adapter that must literally satisfy the standard `Allocator` named requirement, which needs both vocabularies side by side — see `kernel/lib/ktl/include/ktl/allocator.hpp:10-21`.

1. Using `auto` for type inference.

1. Always prefer lambda expressions for short, inline functions, especially when they are used as arguments to algorithms or for defining small function objects.

1. Always prefer `enum class` over traditional `enum` for better type safety and scoping. Enum constants are `CamelCase` (`.clang-tidy`'s `EnumConstantCase`), e.g. `Status::Ok`, `AllocationDirection::TopDown` — not `SCREAMING_SNAKE_CASE`.

1. Always use trailing return types for all function signatures, even for functions which return void. This is universal in the real codebase: `auto init() -> Status;`, `auto bootmem_allocate(...) -> bootmem::Range`.

1. Using `[[nodiscard]]` attribute for functions that return a value that should not be ignored — `ustl::Result` itself is declared `[[nodiscard]]` at the class level (`system/lib/ustl/include/ustl/result.hpp:43`); do the same for any function returning a `Result`/`Status`-like value, not just the type definition.

1. Using `[[maybe_unused]]` attribute for variables, functions, or parameters that may not be used to avoid compiler warnings.

1. Using `nullptr` over `NULL` or `0`.

1. Using `override` and `final` for virtual functions.

1. Using `final` for classes that are not meant to be inherited.

1. Ownership: never reach for `std::unique_ptr`/`std::shared_ptr` in kernel code — they don't exist in this codebase's freestanding vocabulary. Use a raw pointer for non-owning/borrowed access; `ustl::Rc<T>` for single-threaded reference counting; `ustl::Arc<T>` (`= ustl::Rc<T, ustl::sync::ThreadSafePolicy>`) when the object may be shared across CPUs; `ustl::Weak<T>` for a non-owning reference that must not keep the object alive. See "Memory allocation & ownership" below for the full pattern and real examples. `std::unique_ptr`/`std::shared_ptr` are fine *only* in host-toolchain-only code (`kernel/lib/obi/cli`, tests) — never under `kernel/` proper, `arch/`, `phys/`, or freestanding `system/lib` code.

1. Never use the ternary operator (`? :`). Always use an explicit `if`-`else` statement instead. The ternary operator harms readability, makes debugging harder (no place to set a breakpoint on a single branch), and encourages nesting that quickly becomes unreadable.

1. Always use braces `{}` with control-flow statements (`if`, `for`, `while`, `do`-`while`, `switch`), even when the body is a single statement. Omitting braces leads to bugs when a second statement is added later without realizing braces are missing (e.g., Apple's `goto fail` vulnerability).

### Error handling

No exceptions, no RTTI — this is enforced at the build level, not just style:
`build/config/BUILD.gn` defines `config("no_exceptions")` (`-fno-exceptions`)
and `config("no_rtti")` (`-fno-rtti`). Never write `throw`/`catch`,
`dynamic_cast`, or `typeid` in kernel code.

1. Return `ustl::Result<T, E>` (`system/lib/ustl/include/ustl/result.hpp`) from any operation that can fail — or its kernel alias `ktl::Result<T>` (`= ustl::Result<T, ours::Status>`, `kernel/lib/ktl/include/ktl/result.hpp:11`) when the error type is the standard kernel status. Construct successes/failures with the free functions `ustl::ok(value)` / `ustl::ok()` / `ustl::err(error)`, never by hand-building the type.

1. Propagate errors with manual early return — check `is_err()`, then `return x.unwrap_err();` (or fold the error into a different `Status` if the caller needs to add context). There is no `TRY`/`OURS_TRY` macro anywhere in this repo — don't invent one; follow the existing manual idiom, e.g. `kernel/phys/module-loader.cpp:45-47, 59-61, 74-76`.

1. `ours::Status` (`system/public/ours/status.hpp`) is the canonical kernel error enum. Reuse its existing members rather than inventing a parallel error type for a new subsystem, unless that subsystem genuinely needs richer error data than a status code carries.

1. Unrecoverable conditions call `panic(fmt, args...)` (`[[noreturn]]`, `system/public/ours/panic.hpp:65-77`), `unreachable()`, or `unimplement()` — never a bare infinite loop or `abort()`. `panic` degrades to an unformatted `vpanic` under `PHYS_ENVIRONMENT` (no formatting runtime that early in boot) — don't assume `std::format`-style calls work identically in phys code.

### Standard library layering

This repo has three library tiers, not one flat "the STL":

- **`ustl`** (`system/lib/ustl/`) — the freestanding STL replacement: containers, `Result`, `Rc`/`Arc`/`Weak`, intrusive collections, algorithms. Usable from phys/host tools and the kernel proper alike.
- **`ktl`** (`kernel/lib/ktl/`) — a thin kernel-space binding layer over `ustl`: fixes the allocator and fixes `ours::Status` as the `Result` error type, e.g. `ktl::Vec<T>` (`kernel/lib/ktl/include/ktl/vec.hpp:10`, `= ustl::collections::Vec<T, A>`), `ktl::Result<T>`.
- **`gktl`** ("Generic Kernel Tooling Library", `kernel/lib/gktl/`) — sits above both, for kernel-object utilities that aren't about storage/ownership: `gktl::Canary` (corruption detection), init-hooks, radix-tree, range types.

1. Freestanding-safe parts of `std::` are used directly throughout the kernel and are fine to reach for — `std::array`, `std::string_view`, `std::move`/`std::forward`, `std::construct_at`/`std::destroy_at`. Don't avoid these out of a blanket "never use std::" instinct; that instinct only applies to the allocating/exception-shaped parts below.

1. Allocating or exception-shaped `std::` — `std::vector`, `std::string`, `std::unique_ptr`/`std::shared_ptr`, `std::expected`, `<iostream>`, `<filesystem>` — is host-toolchain-only. It shows up in `kernel/lib/obi/cli/*.cpp` and test binaries, never in true freestanding kernel/phys/arch code. Grep for existing usage in the target directory before assuming either way.

### Memory allocation & ownership

1. Dynamic allocation goes through `ktl::kmalloc(size, Gaf gaf[, NnId nid])` / `ktl::kfree(ptr)` (`kernel/lib/ktl/include/ktl/kmalloc.hpp`), backed by a slab/object-cache allocator (`kernel/mm/include/ours/mm/object-cache.hpp`). Never call a bare freestanding allocator without a `Gaf` flag.

1. Placement-new with an allocation-flag argument is the kernel's `new` idiom: `new (gaf) T(...)` / `new (gaf) T[n]()`, backed by `operator new(usize, ktl::Gaf)` (`kernel/lib/ktl/new.cpp:6-8`). Pick a `Gaf` (`kernel/mm/include/ours/mm/gaf.hpp`) that matches the allocation's real constraints (DMA-capable, NUMA node, zeroing) instead of defaulting to a generic flag without checking what's available — e.g. `kernel/lib/ktl/include/ktl/allocator.hpp:26` uses `new (ours::mem::kGafKernel) Element[n]()`.

1. Ownership vocabulary (see also "Ownership" under General style): raw pointer for non-owning access; `ustl::Rc<T>` (single-threaded refcounting, the default) for single-owner-domain sharing; `ustl::Arc<T>` (`= Rc<T, ustl::sync::ThreadSafePolicy>`) when the object crosses CPUs; `ustl::Weak<T>` for a non-owning handle that shouldn't keep the object alive. `ustl::RefCounted<T, Policy>` (`system/lib/ustl/include/ustl/rc.hpp`) is the CRTP mixin that gives a type `retain()`/`release()` — e.g. `class VmAspace: public ustl::RefCounted<VmAspace, ustl::sync::ThreadSafePolicy>` with `ustl::Arc<VmArea> m_root_vma;` (`kernel/mm/include/ours/mm/vm_aspace.hpp:23,112`). Mixing policies is a compile error by design (a `static_assert` in `Rc`'s constructor checks the object's actual threading policy) — don't force-cast around it; pick the right pointer type instead.

1. Intrusive lists use `ustl::collections::intrusive::List` plus the `USTL_DECLARE_HOOK_OPTION`/`USTL_DECLARE_LIST` macros to wire up a member hook (`system/lib/ustl/include/ustl/collections/intrusive/list.hpp`; real usage in `kernel/mm/include/ours/mm/vm_aspace.hpp:117-119`). These macros are a sanctioned exception to "decline to use macros" below — they generate boilerplate no template alone expresses this tersely here.

### Concurrency & locking

1. Every lock type is annotated with a clang thread-safety capability at its declaration: `class TA_CAPABILITY("Mutex") Mutex { ... }` (pattern from `kernel/include/ours/sync/mutex.hpp:16`, `kernel/include/ours/sync/spin-lock.hpp:37`). Any new lock type must do the same.

1. Guarded members are annotated `TA_GUARDED(lock)` at the point of declaration, never left implicit — e.g. `List g_global_caches TA_GUARDED(g_cache_lock);` (`kernel/mm/include/ours/mm/object-cache.hpp:119`). Functions that require a lock held on entry declare `TA_REQUIRES(lock())` (`kernel/include/ours/task/sched/scheduler.hpp`).

1. The only spellings are `TA_CAPABILITY`/`TA_GUARDED`/`TA_REQUIRES` (`system/public/ours/compiler.hpp`). The short forms `TA_CAP`/`TA_GUARD`/`TA_REQUIRE` were compatibility macros for pre-refactor headers; their last uses were converted and the macros deleted, so they no longer compile.

1. Take locks through `lockdep::Guard`/`lockdep::ScopedGuard` (re-exported at `kernel/include/ours/lockdep.hpp:8-14`), not by calling `lock()`/`unlock()` directly. That's what feeds `system/lib/lockdep`'s cycle detection — a lock taken outside a Guard is invisible to lockdep.

### Assertions & invariants

1. Invariants that must hold in every build: `ASSERT(condition, "message", args...)` (`system/public/ours/assert.hpp`) — always panics via `ours::panic` on failure, never compiled out.

1. Invariants that are expensive or only meaningful for debugging: `DEBUG_ASSERT(condition, ...)` — compiles to nothing unless `DEBUG` is defined. Use `DEBUG_ASSERT` for checks with real runtime cost (e.g. an O(n) list-consistency walk); use `ASSERT` for cheap checks that should hold in production too.

1. There is no `CHECK`/`DCHECK` anywhere in this repo — don't introduce Google-style names; the project's macros are the unprefixed `ASSERT`/`DEBUG_ASSERT`.

1. For objects prone to use-after-free or heap corruption, embed a `gktl::Canary<T> m_canary;` member and call `.verify()` on access — this is the repo's convention for catching corruption early on hot kernel objects, e.g. `VmAspace` and `Dispatcher` (`kernel/lib/gktl/include/gktl/canary.hpp:41-78`; usage in `kernel/mm/include/ours/mm/vm_aspace.hpp:98`, `kernel/object/include/ours/object/dispatcher.hpp:53`).

### Namespacing

1. Root namespace is `ours::`, with one nested namespace per subsystem — `ours::mm`, `ours::object`, `ours::sync`, `ours::task::sched`, `ours::phys`, `ours::time`, `ours::irq`. Put new subsystem code in `ours::<subsystem>`, not loose in `ours::` directly.

1. Architecture-specific code lives in its own top-level `arch::`/`arch::paging` namespace, not nested under `ours::`. Architecture selection happens at the GN/build-toolchain level (which arch directory's headers get compiled), not through namespace-based runtime dispatch — see "Build-time polymorphism" below.

1. Library code (`ustl`, `ktl`, `gktl`, `lockdep`, `obi`) stays in its own flat namespace matching its directory. Don't nest a library's namespace under `ours::`.

1. Wrap translation-unit-local helpers in an unnamed `namespace { ... }` block rather than marking them `static` — this is pervasive in the codebase (100+ occurrences under `kernel/`), e.g. `kernel/phys/module-loader.cpp:16-37`. Genuinely internal-but-cross-TU code goes in a nested `priv` or `detail` sub-namespace instead (e.g. `kernel/include/ours/sync/rwlock.hpp`, `kernel/mm/include/ours/mm/pmm.hpp`).

### Object orientation

Model with objects. This is the default for anything with state: a type owns
its data *and* the invariants over that data, and its public interface says
what callers may ask of it, not what it happens to hold. Reach for a free
function only when there is no state to guard.

1. **The object owns its invariant, and it is the only one who does.** If two
   places can each answer "what is this thing's X", one of them will
   eventually answer differently. `Thread::Current::fault()` reads
   `m_user_thread->process().aspace()` while `Scheduler::switch_context()`
   reads `Thread::m_aspace`, so a page fault resolves against one address
   space while CR3 is loaded from another — the same question with two
   owners (`docs/repairs/260910/mm-aspace.md`). The fix is not to keep them
   in sync; it is to have one owner and let the other ask it.

1. **State is private; the interface is verbs.** `PreemptionState`
   (`kernel/include/ours/task/preemption_state.hpp`) keeps its packed counter
   and pending mask private and exposes `disable_preemption()`,
   `enable_preemption()`, `add_pending()` — callers say what they want to
   happen, not which bit to flip. A `struct` whose fields are poked by
   whoever holds it has moved its invariant into every call site, where no
   one is responsible for it.

1. **A constructor establishes the invariant; nothing else may skip it.**
   `DpcRunner::init()` used CpuLocal's zero-filled storage directly, so its
   intrusive list header was `{null, null}` rather than self-pointing, and
   the first enqueue wrote through a null pointer. Zeroed memory is not a
   constructed object. Per-CPU objects in this tree are placement-new'd for
   exactly this reason — `Scheduler::init()` opens with `new (this) Self()`.

1. **"OOP" here does not mean inheritance, and inheritance is not yours to
   choose.** Composition is the default and needs no discussion: reuse comes
   from holding a member and forwarding through it (see "Composition over
   inheritance" above). `VmAspace` *composes* `ArchVmAspace m_arch` rather
   than deriving from it.

   A base class *is* allowed — but **ask the developer before introducing
   one**, every time, even where it looks obviously right. This is a standing
   instruction, not a stylistic preference: put the question with the
   composition alternative spelled out, and let them decide. Do not
   quietly add a base class, and do not contort a design into an awkward
   composition to avoid asking.

   The two shapes that have already been decided, and so do not need asking
   again: a genuinely open, runtime-selected hierarchy (the kernel-object
   `Dispatcher` family, chosen by a syscall argument), and CRTP injecting
   behaviour with no vtable (`ustl::RefCounted<Self, Policy>`). If a base
   class would exist only so two types can share a function body, that is the
   case where a member is the answer — but say so and confirm rather than
   assuming.

1. **Nor does it mean a class per concept.** A "manager" holding no state and
   a class whose methods never touch `this` are namespaces wearing a
   costume — write a namespace, or free functions in a `detail`/`priv`
   sub-namespace. The test is whether there is an invariant to guard.

1. **Prefer a named type over a bare primitive when the primitive has rules.**
   A `CpuNum` that may be `INVALID_CPU_NUM`, an offset that must be page
   aligned, a handle value that carries a generation — each is a place where
   a type can enforce what a comment otherwise has to ask for.

### Build-time polymorphism over virtual dispatch

1. Reach for `virtual` only for genuinely open, runtime-selected hierarchies — the canonical example is the kernel-object `Dispatcher` family (`kernel/object/include/ours/object/dispatcher.hpp:45,51`), where the concrete type is chosen by a syscall argument at runtime. This is intentionally rare — on the order of two dozen headers in the whole kernel use `virtual` at all.

1. Everywhere else — most importantly architecture selection — prefer plain composition where the concrete type is already fixed by which files got compiled for the target, with no template or virtual base needed at all. `VmAspace` composes a plain, non-templated `ArchVmAspace m_arch;` member (`kernel/mm/include/ours/mm/vm_aspace.hpp:105`); `ArchVmAspace` itself (`kernel/arch/x86/mm/include/ours/mm/arch-vm-aspace.hpp` for x86) has zero virtual methods, and the x86/ARM/RISC-V variants never coexist in one binary — GN picks the one header that exists for the active arch, enforced by a `#if !__has_include(<ours/mm/arch-vm-aspace.hpp>) #error` guard in `vm_aspace.hpp:6-8`. This is *not* CRTP and doesn't need to be — the header that gets included is already the right one, so there's nothing left to dispatch on.

1. Use CRTP specifically when behavior needs injecting into many otherwise-unrelated derived types without a vtable. `ustl::RefCounted<T, Policy>` (`system/lib/ustl/include/ustl/rc.hpp`) is the model to follow: every ref-counted kernel object inherits `ustl::RefCounted<Self, Policy>` and gets `retain()`/`release()` resolved entirely at compile time.

### Macros

1. Decline macros for anything expressing *logic* — prefer inline functions, `constexpr` functions, or templates instead. Macros are a powerful but dangerous feature: they're hard to read, hard to debug, and easy to misuse.

1. Decline macros for *constants* too. `constexpr` defines constants; a macro's only sanctioned role as a value is carrying a build-time setting inward, and that setting should be consumed into a typed `constexpr` at its point of definition. See "Constants and configuration" under General style for the full rule and the `HZ` example.

1. Macros remain the sanctioned tool for cross-cutting declarative annotations C++ has no other syntax for: invariants (`ASSERT`/`DEBUG_ASSERT`), thread-safety capabilities (`TA_*`), and intrusive-container hook boilerplate (`USTL_DECLARE_LIST`/`USTL_DECLARE_HOOK_OPTION`). Reuse the existing ones — don't write a parallel macro that does the same job under a different name.

1. C++26 reflection (`std::meta`) is *not* an established alternative to macros in this codebase yet, despite what "annotations after c++26" might suggest. `kernel/lib/gktl/include/gktl/dyn.hpp` and `gktl/experiment.hpp` are explicitly-named exploratory sketches — one doesn't even compile as written — and nothing else in the repo includes either file. Don't build new features on top of them, and don't present them to the user as a supported pattern; if a task genuinely seems to call for that direction, ask the user first rather than extending the sketch unprompted.

### Headers

1. Use `.hpp` for all new C++ headers. `.h` is reserved for the freestanding libc shim (`system/lib/libc/include/*.h`) — never give a new C++ header a `.h` extension.

1. Use classic include guards, not `#pragma once`, for new headers: `OURS_<PATH>_HPP` / `USTL_<PATH>_HPP` / `KTL_<PATH>_HPP` / `GKTL_<PATH>_HPP`, matching the header's path — e.g. `OURS_MM_VM_ASPACE_HPP` for `ours/mm/vm_aspace.hpp`. A minority of existing `gktl`/`arch` headers use `#pragma once`; that's pre-existing inconsistency, not a precedent — don't add to it.

### Tooling

1. Every C++ change must be run through `clang-format` and `clang-tidy` before it's considered done — see `.agents/skills/cpp-write-and-verify` for the exact binaries and paths (the `prebuilt/` LLVM tree) in this environment. Don't restate or improvise those commands here; that skill is the single source of truth for them and drifts if duplicated.

## Gotchas

Corrections that recur, one line each. Add a line whenever the same fix has to be made twice.

- Never wrap something the language already spells in a macro; attribute macros exist only for vendor attributes (`[[gnu::...]]`, `[[msvc::...]]`).
- Attribute macros (`INIT_CODE`, `FORCE_INLINE`) go on their own line above the declaration; `PACKED` is the one exception and stays inline with the type. An attribute macro expands to the attribute only: never fold `inline` into one, write `inline` where the language needs it.
- New constants are `SCREAMING_SNAKE_CASE`, as the checked-in `.clang-tidy` requires. Legacy `kCamelCase` constants are not a template to copy.
- Device-driver families dispatch through a `constexpr` ops table, never `virtual`: the user rejects virtual dispatch there on instruction-cache grounds.
- In ustl's intrusive node algorithms, a policy or observer (tree augmentation, for example) is a struct of function pointers passed as a parameter to the operations that need it, never a class template parameter and never a per-policy template, so the shared algorithm is emitted once. Each such operation comes in a policy-taking and a plain variant.
- Compiler runtime symbols the compiler emits (`__udivti3`, soft-float helpers, wide atomics) come from the toolchain's own library, `libclang_rt.builtins` under LLVM or `libgcc` under GCC. Never hand-write them.
- A bitmask of caller-supplied flags is named `*Options` (`UnmapOptions`, `EnqueueOptions`) and its parameter `options`; never `*Control`.
- Prefer freestanding-safe `std::` facilities (`std::atomic`, `std::memory_order`, `<type_traits>`) over ustl wrappers. The exception is optional values: use `ustl::Option`, which traps on empty access, never `std::optional`.
- Use the prelude integer aliases (`u8` ... `usize`, `kernel/include/ours/prelude.hpp`) instead of `std::size_t` or `std::uint32_t`.
- Source and header file names separate words with underscores, never hyphens.

## Workflow

1. Before adding a new type or utility, check whether `ustl`/`ktl`/`gktl` already has one — this repo deliberately avoids re-deriving container/ownership/result primitives that already exist (see "Standard library layering").
2. Write the code following the Requirements above, matching the immediate surrounding file's conventions where this skill is silent.
3. Run the `cpp-write-and-verify` skill (`clang-format` + `clang-tidy` against the real build's `compile_commands.json`) before calling the change done.

## References

External:

- [cppreference](https://cppreference.com): comprehensive reference for the C++ language, including C++20/23 features.

Repo-internal (ground truth for the rules above — prefer re-reading these over trusting a stale summary if something looks off):

- `system/lib/ustl/include/ustl/result.hpp`, `kernel/lib/ktl/include/ktl/result.hpp`, `system/public/ours/status.hpp` — error handling.
- `system/public/ours/assert.hpp`, `system/public/ours/panic.hpp` — assertions and fatal errors.
- `system/public/ours/compiler.hpp` — thread-safety annotation macros.
- `system/lib/ustl/include/ustl/rc.hpp` — `RefCounted`/`Rc`/`Arc`/`Weak`.
- `kernel/lib/ktl/include/ktl/kmalloc.hpp`, `kernel/lib/ktl/new.cpp`, `kernel/mm/include/ours/mm/gaf.hpp` — allocation.
- `kernel/lib/gktl/include/gktl/canary.hpp` — corruption-detection canaries.
- `kernel/mm/include/ours/mm/vm_aspace.hpp`, `kernel/arch/x86/mm/include/ours/mm/arch-vm-aspace.hpp` — the build-time arch-selection pattern.
- `.clang-tidy`, `.clang-format` — naming and formatting rules this skill must stay consistent with.
