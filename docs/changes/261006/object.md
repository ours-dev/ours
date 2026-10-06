# object changes

## Title: a VMO is the vDSO's when its vDSO code segment is not empty, and the separate m_is_vdso bool is gone (2026-10-06)

**Files**
- `kernel/object/include/ours/object/vmo_dispatcher.hpp` — `bool m_is_vdso` removed; `is_vdso()` reads whether `m_vdso_code` is empty; `mark_vdso()` asserts the segment it is given is not empty

**Symptom**
Latent, cannot be triggered today. `VmoDispatcher` kept two members for one fact: `m_is_vdso`, and `m_vdso_code`, which is meaningful only when the bool is set. The bool cost 8 bytes per VMO: one byte of `bool`, then seven of padding before the 8-byte-aligned `VdsoCode`.

**Cause**
The codebase's rule for yes/no state (modern-cpp-development, the user's rule of 2026-10-03) is that facts about an object go in a Flags word, not in separate `bool` members. The user pointed at this one.

A Flags word with this one bit would still be a word of its own with its own padding, and the class has no other yes/no fact to share it with. The fact needs no storage at all, though. "This VMO is the vDSO's" and "this VMO's vDSO code segment is not empty" are the same fact:

- an unmarked VMO holds a default `VdsoCode`, whose `size` is 0;
- the vDSO always has code, since its one executable segment is what every process's system calls run.

Two members for one fact is also the shape the style skill warns about: one invariant with two owners can answer two ways. Before this change, a `mark_vdso()` given an empty segment would have set the bool, and `map()` would then have required an executable mapping of exactly zero bytes.

**Change**
Before:

```cpp
    auto mark_vdso(VdsoCode const code) -> void {
        m_is_vdso = true;
        m_vdso_code = code;
    }
    auto is_vdso() const -> bool {
        return m_is_vdso;
    }
    bool m_is_vdso = false;
    VdsoCode m_vdso_code;
```

After:

```cpp
    auto mark_vdso(VdsoCode const code) -> void {
        ASSERT(code.size != 0, "the vDSO's code segment is empty");
        m_vdso_code = code;
    }
    auto is_vdso() const -> bool {
        return m_vdso_code.size != 0;
    }
    VdsoCode m_vdso_code;
```

`ASSERT` rather than `DEBUG_ASSERT`: it runs once per boot, and a release kernel that marked an empty segment would refuse every process's vDSO mapping, a failure far from its cause.

Rejected options:

- **A `VmoFlags` word with a `Vdso` bit.** It follows the rule's letter, but a one-bit word saves nothing over the `bool` and still keeps the second fact.
- **`ustl::Option<VdsoCode>`.** Option keeps its own engaged flag, the same `bool` and padding inside another type.

**Reference**
None, this project's own rule. The idea is the one Linux uses in `mm->context.vdso`: a pointer that is null until the vDSO is mapped serves as both the flag and the value.

**Verification**
- The build of `out/bringup.x86` is clean.
- `scripts/format_cpp.py --check` is clean on the header, and `clang-tidy` reports nothing in it.
- `boot_check.py`: dbg PASS (33 passed, 3 skipped, 0 failed), rel PASS (31 passed, 9 skipped, 6/6 required). Every process maps the vDSO at start, so each boot runs `is_vdso()` on the real vDSO VMO, and a wrong answer would fail every process's start.

**Deliberately not done**
The other 35 `bool` members in the tree, outside the third-party trees, are left for a sweep of their own. Each needs its protection checked before it can share a word, and some, like this one, need no storage at all. They are recorded as `docs/repairs/261006/cpp-style.md` item 1.

**How it was found**
The user read `bool m_is_vdso = false;` and pointed at the rule.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does a one-bit Flags word save nothing here, while removing the bool saves 8 bytes per VMO?
- What would `map()` have demanded of a VMO marked with an empty code segment before this change, and what stops that now?
- When is a `bool` member better replaced by a fact the object already holds, rather than by a bit in a Flags word?

## Title: a thread's pending dispatcher deletions are a ustl intrusive Slist, in place of a hand-linked m_next_to_delete pointer (2026-10-06)

**Files**
- `kernel/object/include/ours/object/dispatcher.hpp` — `Dispatcher *m_next_to_delete` replaced by `SlistMemberHook<> m_deletion_hook`, with `DeletionOption` and `DeletionList` declared beside it
- `kernel/object/dispatcher.cpp` — `rc_recycle()` pushes onto and pops from a `DeletionList`

**Symptom**
None in behaviour. `rc_recycle()` kept a stack of objects waiting for destruction as a hand-written singly linked list: a raw `Dispatcher *` in each object, and pointer surgery on a `Dispatcher **` head, for a structure ustl already provides.

**Cause**
The modern-cpp-development workflow's first step is to look in ustl, ktl and gktl before writing a container. ustl has an intrusive singly linked list, `Slist` with `SlistMemberHook`, already used by `gktl/chain_buffer.hpp`. The hand-written list had to be read pointer by pointer to see that it is a stack. It also had no check that an object was off the list before it was destroyed, and a destroyed object still linked would leave the next pop reading freed memory.

The deletion chain itself is unchanged: why it exists is in the comment on `rc_recycle()`. A dispatcher can hold the last reference to another, and a channel end can be sent through a channel a thousand deep, so destruction runs as a loop and not as a recursion.

**Change**
Before:

```cpp
    auto *const pending = static_cast<Dispatcher **>(thread->recursive_object_deletion_list());
    if (pending != nullptr) {
        object->m_next_to_delete = *pending;
        *pending = object;
        return;
    }
    Dispatcher *list = nullptr;
    thread->set_recursive_object_deletion_list(&list);
    while (object != nullptr) {
        object->rc_finish();
        object = list;
        if (object != nullptr) {
            list = object->m_next_to_delete;
        }
    }
```

After:

```cpp
    auto *const pending = static_cast<DeletionList *>(thread->recursive_object_deletion_list());
    if (pending != nullptr) {
        pending->push_front(*object);
        return;
    }
    DeletionList list;
    thread->set_recursive_object_deletion_list(&list);
    object->rc_finish();
    while (!list.empty()) {
        auto next = list.pop_front();
        next->rc_finish();
    }
```

The order is the same: last in, first out.

The hook is in SafeLink mode, ustl's default. It starts unlinked, and its destructor requires that it is unlinked. Every object is popped before `rc_finish()` destroys it, so the check holds, and an object destroyed while still on a list now panics at the destruction instead of corrupting the next pop. The thread still keeps an untyped `void *` to the list, so the task layer does not name an object-layer type.

Size: the hook is one pointer, as `m_next_to_delete` was, so `Dispatcher` does not grow. The list head is a local of the outermost `rc_recycle()` and keeps a count beside its node.

**Reference**
The same pending list, by different means. Zircon's `Dispatcher::fbl_recycle()` defers through the thread's `recursive_object_deletion_list_` and links the waiting objects through an intrusive singly linked list node in the dispatcher.

**Verification**
- The build of `out/bringup.x86` is clean.
- `scripts/format_cpp.py --check` is clean on both files.
- `clang-tidy` reports nothing on the changed lines; its `modernize-use-override` on `~SoloDispatcher()` was there before.
- `boot_check.py`: dbg PASS (33 passed, 3 skipped, 0 failed), rel PASS (31 passed, 9 skipped, 6/6 required). Both logs have "shell: a 1000-deep chain of ends, closed": the shell test that sends a channel end through a thousand channels and closes the outermost runs every pending deletion through the new list.

**Deliberately not done**
None.

**How it was found**
The user asked why the chain exists and whether `ustl::intrusive::Slist` could replace it.

**The user's prediction**
None.

**Three questions you should be able to answer**
- At what moment is a dispatcher pushed onto the list, and why does pushing it need no lock?
- Why must `rc_recycle()` pop an object before calling `rc_finish()` on it, and what does the SafeLink hook do if it does not?
- Why does the thread keep the list as `void *` rather than as a `Dispatcher::DeletionList *`?

## Title: a thread points at its pending deletions through a typed object::PendingDeletions *, registered by a PendingDeletionsScope, in place of a void * set and cleared by hand (2026-10-06)

**Files**
- `kernel/include/ours/task/thread.hpp` — forward-declares `object::PendingDeletions`; `recursive_object_deletion_list()` and `set_recursive_object_deletion_list()` replaced by `pending_deletions()` and `Thread::PendingDeletionsScope`; the member is `object::PendingDeletions *m_pending_deletions`
- `kernel/object/include/ours/object/dispatcher.hpp` — new `struct PendingDeletions` holding a `Dispatcher::DeletionList`, a friend of `Dispatcher` so it can name the private list type
- `kernel/object/dispatcher.cpp` — `rc_recycle()` opens a `PendingDeletionsScope` instead of setting and clearing the pointer

**Symptom**
Latent, cannot be triggered today. The thread kept the pending list as a `void *`, and `rc_recycle()` set it before its loop and cleared it after. Two things could go wrong without a word from the compiler:

- a caller could cast the `void *` to the wrong type;
- a later early return between the set and the clear would leave the thread pointing at a stack frame that no longer exists. The next deletion on that thread would then push onto it.

**Cause**
Two separate gaps.

- **The type.** `void *` was there so that the task layer would not depend on the object layer, which sits above it. An incomplete type does the same job and keeps the type: `thread.hpp` forward-declares `object::PendingDeletions`, as it already forward-declares `object::ThreadDispatcher`, and only the object layer defines it. The `static_cast` from `void *` disappears, and passing anything else is a compile error.
- **The pairing.** Registering the list and withdrawing it is a paired state change. The style skill's rule (2026-10-04) says such a pair is a scope with an RAII object, never a call and its matching undo, because a later edit can separate them.

The pointer itself stays a raw pointer on purpose. The thread does not own the list, which is a local of the outermost `rc_recycle()`; the thread only borrows it for that call's length. The ownership vocabulary says exactly that with a raw pointer, and an `Arc` or `Box` would claim an ownership that does not exist.

**Change**
Before:

```cpp
    auto *const pending = static_cast<DeletionList *>(thread->recursive_object_deletion_list());
    ...
    DeletionList list;
    thread->set_recursive_object_deletion_list(&list);
    ...
    thread->set_recursive_object_deletion_list(nullptr);
```

After:

```cpp
    auto *const pending = thread->pending_deletions();
    ...
    PendingDeletions deletions;
    task::Thread::PendingDeletionsScope const scope{*thread, deletions};
    ...
```

The scope is declared after the list, so it is destroyed first: the thread stops pointing at the list before the list goes. The scope's constructor is `[[nodiscard]]`, so an unnamed temporary scope, which would register and withdraw at once, is a compile error. The setter is gone, so the scope is the only way to register a list.

Rejected options:

- **Include the object header in `thread.hpp`.** It would invert the layering for one pointer.
- **Store the list in the thread itself.** The thread would then need the complete list type, which is the same inversion.

**Reference**
Zircon keeps the same pointer on its thread as `recursive_object_deletion_list_` and sets and clears it around the loop in `Dispatcher::fbl_recycle()`. The type and the scope are this project's own rules.

**Verification**
- The build of `out/bringup.x86` is clean, with the thread-safety analysis on.
- `scripts/format_cpp.py` was run on all three files.
- `clang-tidy` reports nothing on the changed lines; its `modernize-use-override` on `~SoloDispatcher()` was there before, and only moved down.
- `boot_check.py`: dbg PASS (33 passed, 3 skipped, 0 failed), rel PASS (31 passed, 9 skipped, 6/6 required); both logs have "shell: a 1000-deep chain of ends, closed", which runs the pending list through the scope.

**Deliberately not done**
None.

**How it was found**
The user read the two accessors and asked whether the raw pointer could go.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is the thread's pointer still a raw pointer, and what would an `Arc` there claim that is false?
- What does declaring `scope` after `deletions` guarantee, and what would go wrong with the order reversed?
- How does an incomplete type let `thread.hpp` name an object-layer type without the task layer depending on the object layer?

## Title: an exceptionate's shutdown is a bit of a Flags word guarded by its lock, in place of bool m_is_shutdown (2026-10-06)

**Files**
- `kernel/object/include/ours/object/exceptionate.hpp` — `bool m_is_shutdown` replaced by `Flags m_flags TA_GUARDED(m_lock)`, with a private `enum class Flags { None, Shutdown }`
- `kernel/object/exceptionate.cpp` — `set_channel()` tests `Flags::Shutdown` and `shutdown()` sets it

**Symptom**
Latent, cannot be triggered today. Two things were wrong with the member:

- It was a `bool`, against the rule for yes/no facts.
- Its only protection was a comment, "Guarded by m_lock". The thread-safety analysis did not know it, so an access without the lock would have compiled.

**Cause**
The modern-cpp-development rule of 2026-10-03 asks for an object's yes/no facts to share an `enum class` Flags word. The style skill also asks for every guarded member to be annotated `TA_GUARDED(lock)` at its declaration, never left to a comment. The user pointed at this member.

The rule's condition holds: the word's one bit is written only under `m_lock`, in `shutdown()`, and read only under it, in `set_channel()`, the same protection the `bool` had. The word is a `u8`, the size of the `bool`, so the layout does not change. The gain is the annotation, and a place for the next fact the exceptionate keeps.

**Change**
Before:

```cpp
    /// Guarded by m_lock.
    bool m_is_shutdown = false;
```

After:

```cpp
    enum class Flags : u8 {
        None,
        Shutdown = 0_bit,
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(Flags);

    Flags m_flags TA_GUARDED(m_lock) = Flags::None;
```

`set_channel()` reads `!!(m_flags & Flags::Shutdown)`, and `shutdown()` writes `m_flags |= Flags::Shutdown`, both under `Guard guard{m_lock}`, which the analysis now checks.

**Reference**
None, this project's own rule. Zircon's `Exceptionate` keeps the same fact as `bool is_shutdown_ TA_GUARDED(lock_)`.

**Verification**
- The build of `out/bringup.x86` is clean, with `-Werror` and the thread-safety analysis on, which now checks both accesses.
- `scripts/format_cpp.py` was run on both files.
- `clang-tidy` reports nothing in either file.
- `boot_check.py`: dbg PASS (33 passed, 3 skipped, 0 failed), rel PASS (31 passed, 9 skipped, 6/6 required). The shell's exception checks run, among them a probe killed after its page fault went through an exception channel.

**Deliberately not done**
`m_channel`, `m_thread_rights` and `m_process_rights` carry the same "Guarded by m_lock" comment without `TA_GUARDED`. Annotating them is the same fix, but it is outside the member the user pointed at. It is folded into `docs/repairs/261006/cpp-style.md` item 1, whose count drops to 34 with this member.

**How it was found**
The user read `bool m_is_shutdown = false;` and asked for an `enum class` Flags.

**The user's prediction**
None.

**Three questions you should be able to answer**
- What condition lets the shutdown bit share a word with facts added later, and which bits could never join it?
- The word is the same size as the `bool`. What did the change gain, if not bytes?
- What does `TA_GUARDED(m_lock)` catch that the comment "Guarded by m_lock" did not?
