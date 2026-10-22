# ustl changes

## An any member hook is tested in a singly linked list (2026-10-23)

**Files**
- `system/lib/ustl/test/set_any_hook_test.cpp` — the same item and hook, after the Set and the List, in an `Slist`

**Symptom**
None: a test. The kernel's list of timers to restart became an `Slist` (`docs/changes/261023/kernel-time.md`), and the any hook had only been tried in a Set and a List.

**Cause**
`AnyToSlistHook` was in `any_hook.hpp` and generic over the node traits, so it should work with the container-side value traits added on 2026-10-22, and nothing had run it.

**Change**
The test pushes three items at the front, takes the middle one out through `iterator_to()` and `erase()`, pops the front, puts the popped item into the Set, and clears. It runs on the circular `Slist`, the default.

It found one property worth writing down. An `Slist<..., Linear<true>>` with the safe-link any hook cannot tell its last node from an unlinked one: a linear list ends in a null link, and the hook's `is_linked()` is "the link is not null". The first version of the case asked for a linear list and failed on the first push (`is_linked()` false for a node that had just been linked). The case uses the circular list and says why in a comment; it does not assert the linear behaviour, which is a limit of the hook and not a promise.

**Reference**
None, this project's own design: `linear_slist_algorithms.hpp` and the hook in `any_hook.hpp`. I did not compare Boost.Intrusive's behaviour of a linear `slist` in safe-link mode.

**Verification**
`set-any-hook-test` passes (host test, part of the 146). The first version of the case, which asked for `Linear<true>`, returned code 13 at the push; the case as committed uses the circular list and returns 0.

**Deliberately not done**
A test of the linear behaviour, and a change to the hook so that a linear list's last node reads as linked. The hook has no second bit to say so.

**How it was found**
The test returned code 13 at the push; the check was split into one `if` per fact to see which one was false.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. How does the any hook decide a node is linked, and why does that fail for the last node of a linear list?
2. What does a test of an Slist prove that the Set and List cases did not?
3. Why does the case pop an item and put it in a Set straight after?

## The intrusive Set looks elements up by a key that is not an element (2026-10-23)

**Files**
- `system/lib/ustl/include/ustl/collections/intrusive/set.hpp` — `lower_bound(key, compare)` and `upper_bound(key, compare)`, mutable and const, forwarding to the tree
- `system/lib/ustl/test/set_test.cpp` — a case that looks a Set up by a bare `int`

**Symptom**
None: a missing interface. `Set` could only be searched with an element (`upper_bound(T const &)`), while `Multiset` and the underlying `RBTree` already took a key and a comparison.

**Cause**
A set keyed on a field of a big or non-copyable element has no element to search with. The page source's set of outstanding requests (`docs/changes/261023/mm.md`) is keyed on each request's end offset, and a lookup asks "the first request whose end is past this offset": the caller has an offset, not a request, and a request lives on a faulting thread's stack and cannot be made up for a search.

**Change**
Four forwarding members, the same signatures `Multiset` has:

```cpp
template <typename Key, typename Compare>
constexpr auto upper_bound(Key const &key, Compare const &compare) noexcept -> IterMut {
    return m_tree.upper_bound(key, compare);
}
```

They return an iterator, as `Multiset`'s do, not the `Option<T &>` of the element-taking lookups, because the caller walks on from the result.

**Reference**
Boost.Intrusive's `set::upper_bound(const KeyType &, KeyTypeKeyCompare)`.

**Verification**
`set-test` passes (host); the new case returns 8 on failure. Used by `pagesource.test` through `PageSource`.

**Deliberately not done**
`find(key, compare)` and `count(key, compare)`: nothing needs them yet.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why does a lookup by key need a comparison with the key on one side and the element on the other, and which side for `upper_bound`?
2. Why can a set keyed on end offsets answer "which request contains this offset" with one `upper_bound`?
3. What would go wrong if two elements of a `Set` compared equal on the key?
