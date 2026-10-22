# ustl changes

## An any hook kept in a member can be used by a Set or a List (2026-10-22)

**Files**
- `system/lib/ustl/include/ustl/collections/intrusive/any_hook.hpp` — `AnyMemberValueTraits`, and `AnyToSomeHook` makes it from a `MemberHook` option
- `system/lib/ustl/test/set_any_hook_test.cpp`, `system/lib/ustl/BUILD.gn` — a host test, `set-any-hook-test`, in the `tests` group

**Symptom**
`AnyMemberHook` and the adaptors `AnyToSetHook`, `AnyToListHook` and the others existed and had a test, and no container could use them. The same program, compiled in a scratch file with the kernel's freestanding flags:

```cpp
struct Item { in::AnyMemberHook<> hook; int key; /* operator< */ };
using HookOpt = in::MemberHook<Item, decltype(Item::hook), &Item::hook>;
in::Set<Item, in::AnyToSetHook<HookOpt>> s;     // error
in::Set<Item, HookOpt> t;                       // error
```

The first stopped in `detail/value_traits_access.hpp:14` with `no type named 'NodeTraits'`; the second with `no member named 'set_parent'` (and `set_color`, `Color`) in `AnyNodeTraits<void *>`.

**Cause**
Half of the feature was written. The hook side is there: an any node, one set of node traits for each kind of container, and the adaptors that say which of them a container wants. The container side was not: a container asks the option it was given for a value traits (`NodeTraits`, `to_node_ptr()`, `to_value_ptr()`, `LINK_MODE`), and `AnyToSomeHook::Pack::ProtoValueTraits` handed it a record of tags (`IS_ANY_HOOK`, `BasicHookType`, `NodeTraitsFromVoidPointer`) that nothing in the tree read. The names `IS_ANY_HOOK` and `BasicHookType` appeared only in `any_hook.hpp` itself. Without the adaptor, the member hook's value traits take the node traits from the hook, which for an any hook are the generic `AnyNodeTraits` with none of the operations a tree or a list needs.

An any hook is one node (`AnyNode`: three pointers and a word) on which the algorithms of any container can work; which fields mean "parent" or "next" is the node traits' business, and those belong to the container. So what has to change with the container is the node traits and nothing else: the node, the pointer to it and the conversion between a value and its node are the same.

**Change**
`AnyMemberValueTraits<Inner, NodeTraitsTemplate>`: `Inner` is the `Mhtraits` that the `MemberHook` option already makes for the member, which knows the member pointer and converts a value to its node and back. This forwards `to_node_ptr()` and `to_value_ptr()` to it and names `NodeTraitsTemplate<VoidPointer>` as its `NodeTraits`. `Mhtraits` is `final`, so it is held and forwarded to and not derived from. A `static_assert` checks that the node and the pointers of the container's node traits are the same types as the hook's.

```cpp
template <typename BasicHook, template <typename> class NodeTraitsTemplate>
    requires requires { typename BasicHook::MemberValueTraits; }
struct AnyProtoValueTraits<BasicHook, NodeTraitsTemplate> final {
    using Type = AnyMemberValueTraits<typename BasicHook::MemberValueTraits, NodeTraitsTemplate>;
};
```

An adaptor given a `MemberHook` option takes this; one given a base hook keeps the record it had (`any_hook_test` checks it), since nothing needs an any base hook with a container and the user asked for the member form.

**Reference**
Boost.Intrusive `boost/intrusive/detail/get_value_traits.hpp`, where the container's value traits are chosen by `is_any_hook` (`any_or_concrete_value_traits`, `AnyHookBaseValueTraits`), and `boost/intrusive/any_hook.hpp`, `any_to_some_hook`, which ustl's adaptor follows. What was missing is the dispatch in the first.

**Verification**
- `set-any-hook-test` (exit 0): a `Set` of items with an `AnyMemberHook`, inserted out of order and read back in order, a duplicate refused and left unlinked, `find`, `iterator_to`, `erase` leaving the item unlinked and the others linked; then the same item's hook in a `List` after the `Set` let go of it; then `clear()` of both leaves every hook unlinked.
- The first version of the test put an item in the `List` while it was still in the `Set`; it trapped, in `detail::require` of the safe link mode, as it should: an any hook is one node and an item is in one container at a time. The test was wrong, not the hook, and now erases first.
- `ours-verify` on the default build: dbg PASS (39 passed, 4 skipped, 0 failed, 0 error lines), rel PASS (37 passed, 8 skipped, 0 failed, 6/6 required); host-tests PASS (146, one more than before; `set-test`, `any-hook-test`, `set-hook-test` and the list tests among them).
- `scripts/format_cpp.py`, `scripts/format_gn.py`, `clang-tidy -p out/bringup.x86` on the test: no diagnostic in it.
- Not verified: the other container kinds with an any member hook (`AnyToSlistHook`, `AnyToAvlSetHook`, `AnyToBsSetHook`, `AnyToUnorderedSetHook`); they are made the same way, and only `Set` and `List` are tested.

**Deliberately not done**
- An any **base** hook with a container.
- A negative test of the safe link trap: it ends the process.

**How it was found**
By wanting to use the hook: the first thing tried was `Set<Item, AnyToSetHook<MemberHook<...>>>` in a scratch file, to see whether the instruction to give `Timer` an any member hook was possible before planning it. It was not, and the two errors above located the missing half.

**The user's prediction**
Not given.

**Three questions you should be able to answer**
1. What changes with the container when the same any hook is used by a `Set` and by a `List`, and what stays the same?
2. Why can the new value traits not simply derive from `Mhtraits`, and what does it do instead?
3. Why does putting an item that is in a `Set` into a `List` trap, and where is the check?
