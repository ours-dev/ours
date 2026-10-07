# system-lib changes

## Title: system/lib's bool members: vfs and eftl read theirs from what they hold, and every lone one says why it stays (2026-10-07)

**Files**
- `system/lib/vfs/src/tree.cpp` — `Segments` holds `ustl::Option<std::string_view> m_rest` in place of a view and `bool m_done`
- `system/lib/eftl/include/eftl/elf/version_binding.hpp` — the private `SelectedVersion {m_index, m_hidden}` and its `version_at()` removed: both resolvers decode the index and the hidden bit from the raw versym entry; `VersionBinding::m_hidden` says why it stays
- `system/lib/eftl/include/eftl/elf/relocations.hpp`, `system/lib/eftl/include/eftl/elf/mapped_memory.hpp` — `RelocationObserver::m_allow_symbolic_tables` and `MappedMemory::m_backing_is_valid` say why they stay
- `system/lib/logz4/src/log.cpp` — `LineBuilder::m_dropped` says why it stays
- `system/lib/ustl/include/ustl/collections/detail/btree.hpp` — `BTreeNodeView::m_leaf` and `Node::m_leaf` say why they stay
- `system/lib/ustl/include/ustl/collections/intrusive/bstree_algorithms.hpp`, `.../intrusive/detail/hashtable_node.hpp`, `system/lib/ustl/test/btree_extension_test.cpp` — `InsertCommitDataT::m_link_left`, `HashNode::m_linked` and the test visitor's `m_stop_instead_of_skip` say why they stay
- `system/lib/ustl/include/ustl/option.hpp`, `system/lib/ustl/include/ustl/result.hpp` — the discriminants `m_is_some` and `m_is_ok` (twice) say why they stay

**Symptom**
Latent, cannot be triggered today. Fourteen `bool` members in system/lib against the rule of 2026-10-03 (modern-cpp-development): two stored a fact the object could read from what it already held, and twelve stayed `bool` with no comment saying why.

**Cause**
The rule asks, of every `bool` member: derive it if it restates a fact the object already holds; otherwise give it a bit of the class's Flags word; keep a lone `bool` only with a comment saying why.

- `vfs`'s `Segments::m_done` said "the last segment has been returned", beside a view of what was left. An empty view could not stand for it: `"a/"` ends in an empty segment, which `next()` must still return, so "empty" and "finished" are different states. An optional view says both: a value is what is left, possibly empty, and none is finished. `next()` returns the last segment with `m_rest.take()`, which hands the value out and leaves none in one step, where the old code set the flag and returned the view as two.
- `eftl`'s private `SelectedVersion` was two fields decoded from one ELF word: a versym entry is a 15-bit version index and a hidden bit. It existed only to carry them from `version_at()` to its two callers. The callers now take the raw entry and decode each field with `Versions::version_index()` and `Versions::is_hidden()`, the way `resolve_base()` already did; the struct and its `bool` go.
- The twelve that stay are each their object's only yes/no fact, and none can be read from the rest of the object:
  - `ustl::Option::m_is_some` and `ustl::Result::m_is_ok` are the discriminant of their type's representation, which the repair item leaves out of scope;
  - `BTree::Node::m_leaf` is fixed when the node is made, before it has children: a new root made by `create_node(false)` gets its first child only at `set_root_child()`, so `m_children[0]` would answer "leaf" for it until then, and deriving the flag would make every split and merge responsible for keeping child 0 set at each moment someone asks;
  - `HashNode::m_linked` cannot be read off `m_next`, which is null both for an unlinked node and for the last node of a bucket chain;
  - `MappedMemory::m_backing_is_valid` cannot be read off `m_memory`, which shrinks a null buffer to size 0 so that it looks like an honest empty one;
  - `LineBuilder::m_dropped` cannot be read off the cursor: a line that exactly fills the buffer dropped nothing;
  - `VersionBinding::m_hidden` cannot be read off `m_index`, which has the bit masked off;
  - `RelocationObserver::m_allow_symbolic_tables`, `InsertCommitDataT::m_link_left`, `BTreeNodeView::m_leaf` and the test visitor's option are a caller's option, a check's answer, or a copy handed to an observer.

**Change**
vfs, before:

```cpp
        auto next() -> ustl::Option<std::string_view> {
            if (m_done) {
                return ustl::NONE;
            }
            auto const slash = m_rest.find('/');
            if (slash == std::string_view::npos) {
                m_done = true;
                return ustl::some(m_rest);
            }
            ...
        std::string_view m_rest;
        bool m_done = false;
```

After:

```cpp
        auto next() -> ustl::Option<std::string_view> {
            if (m_rest.is_none()) {
                return ustl::NONE;
            }
            auto &rest = m_rest.value();
            auto const slash = rest.find('/');
            if (slash == std::string_view::npos) {
                return m_rest.take();
            }
            ...
        ustl::Option<std::string_view> m_rest;
```

eftl, before and after, in each of the two resolvers:

```cpp
        auto const version = version_at(versions, symbol_index);
        ...
                .m_index = version->m_index,
                .m_hidden = version->m_hidden,
```

```cpp
        auto const version = versions.version_at(symbol_index);
        ...
        auto const index = Versions::version_index(version.value());
        auto const hidden = Versions::is_hidden(version.value());
        ...
                .m_index = index,
                .m_hidden = hidden,
```

**Reference**
None, this project's own rule. The versym entry's layout, `VERSYM_HIDDEN` in bit 15 above a 15-bit index, is the GNU symbol-versioning format eftl reads.

**Verification**
- The build of `out/bringup.x86` is clean; `clang-tidy` reports nothing in `tree.cpp` or `log.cpp`.
- Host tests: all 93 binaries under `host/obj/system/lib/ustl`, `vfs` and `logz4` pass.
- eftl: its test targets do not compile in this build (each fails on `ours/types.hpp` without the prelude, before and after this change). Built by hand for the host with the build's own flags and `-include kernel/include/ours/prelude.hpp`: `version_binding_test` exit 0 and `mapped_memory_test` exit 0. `relocations_test` exits 51 both with this change and with the header at HEAD, so the failure predates it; the change there is a comment. `dynamic_linkage_test` does not compile (`ByteRange` unqualified), also before this change.
- `boot_check.py`: dbg PASS (33 passed, 3 skipped, 0 failed), rel PASS (31 passed, 9 skipped, 6/6 required, `pc-ps2: key a` and `key A` present). On rel, init serves its namespace through a vfs tree (`boot_server.cpp`), and `routing_checks: passed`, one of the six required lines, comes from opening paths in it.

**Deliberately not done**
- `VersionBinding` could hold the raw versym entry and offer `index()` and `is_hidden()`, which would derive both fields; that changes a public type its tests read field by field, for no reader that needs it.
- The eftl test targets that do not compile, and `relocations_test`'s exit 51, are outside this item. They are reported here rather than fixed.

**How it was found**
`docs/repairs/261006/cpp-style.md` item 1.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why could an empty `std::string_view` not mean "finished" in `Segments`?
- When `insert_value()` grows the tree by one level, for how long does the new root have no child, and what would a leaf flag derived from `m_children[0]` say about it then?
- Why does `HashNode` need `m_linked`, when a list node elsewhere can tell it is linked from its own pointers?
