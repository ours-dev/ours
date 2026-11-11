# mm changes

## The zone's block transfer is named alloc_frames and free_frames (2026-11-10)

**Files**
- `kernel/mm/include/ours/mm/pm_zone.hpp` — `take_block()` and `return_block()` become `alloc_frames()` and `free_frames()`; their comments say which layer they are
- `kernel/mm/pm_zone.cpp` — the two definitions
- `kernel/mm/pm_node.cpp` — the two calls into the zone
- `kernel/mm/include/ours/mm/cpu_local.hpp` — a comment that named `PmZone::take_block()`
- `kernel/tests/pm_zone_test.cpp`, `kernel/tests/pm_node_test.cpp`, `kernel/tests/page_reclaim_test.cpp` — seven calls

**Symptom**
None. A rename; nothing behaves differently.

**Cause**
The allocation path has three layers, and each used a different verb for the same act: `mm::alloc_frames()` at the seam, `PmNode::take_block()` at the node and `PmZone::take_block()` at the zone, with `return_block()` on the way back. The second verb was chosen on 2026-11-04 to tell the public operation, which admits and zeroes, from the transfer of a block out of stock. That distinction is real, but a name is the wrong tool for it: the layers under the seam are meant to be unreachable from outside the PMM, and access control says "this is not the entry" in a way the compiler checks. With access doing that job, a second vocabulary only makes a reader learn two words for one thing and ask what the difference is.

The redesign (`.agents/tasks/pmm-redesign/DESIGN.md`, decision E3) therefore gives every layer the seam's own pair. What differs between the layers is what each does under the name: the seam resolves the request and prepares the frame, the node chooses a zone, the zone moves the block under its lock.

**Change**
The zone's two methods are renamed and nothing else:

```cpp
// before
auto take_block(usize order) -> ustl::Option<PmFrame &>;
auto return_block(PmFrame &frame, usize order) -> void;

// after
auto alloc_frames(usize order) -> ustl::Option<PmFrame &>;
auto free_frames(PmFrame &frame, usize order) -> void;
```

The node's `take_block()` and `return_block()` keep their names in this step. `PmNode` still has the static `alloc_frames(Gaf, usize, NodeMask const &)`, and a member with the same parameters cannot overload a static one, so the node is renamed in the step that removes the static layer (decision D1).

**Reference**
None, this project's own naming. For contrast, Linux uses a different name at each depth (`alloc_pages()`, `get_page_from_freelist()`, `rmqueue()`), and Zircon one name behind its single node (`pmm_alloc_page()` to `PmmNode::AllocPage()`).

**Verification**
`verify` in the `feat/pmm-redesign` worktree, with the counts of the commit before:

| | Before | After |
|---|---|---|
| dbg | 46 passed, 5 skipped, 0 failed | 46 passed, 5 skipped, 0 failed |
| rel | 44 passed, 8 skipped, 0 failed, 7/7 required | 44 passed, 8 skipped, 0 failed, 7/7 required |
| host tests | 183 passed | 183 passed |

`ktest PmZone.*:PmNode.*:PageReclaim.*` on dbg: 3 ran, 3 passed. `format_cpp.py --check` is clean on the seven files, and clang-tidy reports nothing on the renamed lines of `pm_zone.cpp` and `pm_node.cpp`.

**Deliberately not done**
- The methods stay public. Making them private needs a way for `PmZone`'s own tests to reach them, which is decided with the node's layer.
- `free_frames()` keeps its `order` parameter. Reading the order from the block's head frame is decision F2 and a change of contract, not of name.
- No `MigrateType` parameter yet. It arrives with `Gaf::Movable`, which is what would fill it.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why can access control replace a naming convention for "this is not the entry", and what does the compiler check that a name does not?
- Why could `PmNode::take_block()` not be renamed in the same commit?
- The three layers now share one name. What does each do under it that the others must not?
