# phys-fdt changes

## Title: a flattened device tree reader, //kernel/phys/lib/fdt, tested on the host (2026-10-27)

**Files**
- `kernel/phys/lib/fdt/include/fdt/format.hpp` — what the format fixes: magic, version, header field offsets, the five tokens, alignments, big-endian loads
- `kernel/phys/lib/fdt/include/fdt/tree.hpp` — `fdt::Tree` (checked blob, reservations, path lookup), `fdt::Node` (name, property, children), `fdt::Property` (u32, u64, string, cells, `reg`)
- `kernel/phys/lib/fdt/BUILD.gn` — the header target and the two host tests
- `kernel/phys/lib/fdt/test/tree_smoke.cpp` — blobs built by hand: a well-formed one read every way, then broken one way at a time
- `kernel/phys/lib/fdt/test/qemu_virt_smoke.cpp`, `test/data/qemu_virt.dtb` — the tree QEMU's `virt` machine dumps, as a fixture
- `BUILD.gn` — `//kernel/phys/lib/fdt:tests` joins the default group

**Symptom**
None, step 3 of the riscv port. The boot shim receives the physical address of a device tree in `a1` and had nothing that could read it. The tree is how a riscv machine says where its RAM is and where the boot package was put, so the next stage cannot start without a reader.

**Cause**
None, an addition. What it settles:

- The blob is input from firmware, so it is not trusted. A reader that follows an offset or a length it was told, without comparing it to the block it lies in, reads wherever a bad blob points. Every read here goes through `detail::Blocks::item_at()`, which compares against the block first and reports failure instead of reading on; `Tree::from()` walks the whole structure block once through it, so a malformed blob is refused up front, and the accessors that come later use the same checks, so they stay in bounds even if the memory changed after the walk.
- The format has three blocks behind a 40-byte big-endian header. The structure block is a stream of tokens, `BeginNode` (then a NUL-terminated name, padded to 4), `EndNode`, `Prop` (then length, an offset into the strings block, and the value, padded to 4), `Nop` and `End`; nodes nest by `BeginNode`/`EndNode`. A property's value is raw bytes, and what they mean comes from the binding: `reg` is a run of (address, size) pairs whose widths are the *parent's* `#address-cells` and `#size-cells`, which is why `Property::reg()` takes them as arguments and `Node::address_cells()` exists.
- Tools write a node's properties before its children (the specification's text on this was not re-read for this change; libfdt's property walk is understood to assume the same order), so a node's property search stops at its first child. A `late` property after a child is not seen, and the test pins that.
- Walking never recurses. Children are found by scanning tokens and skipping a child's subtree with a depth counter, so a deep or hostile tree costs time, not stack. `MAX_DEPTH` (32) is a sanity bound, not a stack guard.

**Change**
The decisions were the user's (2026-10-27): the shim, not physload, reads the tree (the contract with the kernel stays "hand over what cannot be re-derived"), and the reader is written here rather than taken from a library.

Public shape:

```cpp
auto tree = fdt::Tree::from(data, size);            // Result<Tree>; refuses a bad blob
auto memory = tree.unwrap().find("/memory");        // Option<Node>; "memory" matches memory@80000000
auto reg = memory.value().property("reg").value().reg(2, 2, 0);   // Result<RegEntry{address, size}>
auto cmd = tree.unwrap().find("/chosen").value().property("bootargs");   // Option<Property>
```

Choices that matter later:

- Failures carry a status: `InvArgs` for a malformed blob, `OutOfRange` for a bound (depth, a cell index), `NotSupported` for a version or cell width this reader does not decode.
- A node and a property borrow the blob and are copied freely, a few words each. Nothing is allocated.
- A path component without a unit address matches the first node that has one. `find("/memory")` therefore returns the first of several `memory@...` nodes; a caller that needs all of them walks `children()` and tests `device_type`, which the golden test does.
- `as_string()` accepts exactly one string. `compatible` is a list of strings, and a reader that quietly returned its first entry would hide the others.

Rejected:
- Taking the version from the header only: a blob of version 16 has no structure-size field, so this reader requires 17 and accepts anything whose `last_comp_version` is not newer than 17.
- Recursion for `find()` and subtree skipping. It reads naturally and costs stack in a stage that has a few KiB of it.
- Checking that properties precede children during the walk. A blob a tool wrote has that order; the reader simply does not look past a child for a property, and a blob that breaks the order loses the late property rather than being refused.

**Reference**
The Devicetree Specification, section 5 (the flattened devicetree format): header, memory reservation block, structure block tokens, strings block. In Linux the same job is `fdt_check_header()`, `fdt_next_node()`, `fdt_get_property()` and `fdt_subnode_offset()` of libfdt, and Zircon has its own `devicetree` library under `zircon/system/ulib`. Neither was read for this change; the design follows the specification, and the unit-address-less path match follows what `fdt_subnode_offset_namelen()` is known to do (from memory, not checked).

**Verification**
- `ninja -C out/bringup.x86 host/obj/kernel/phys/lib/fdt/tree-smoke host/obj/kernel/phys/lib/fdt/qemu-virt-smoke`, then both run from the build directory: exit 0.
- The golden fixture was dumped with `qemu-system-riscv64 -machine virt,dumpdtb=... -m 256M -smp 1 -initrd <4096-byte file>`: 5120 bytes, version 17. It is read for the RAM node (`reg` = 0x80000000, 0x10000000), `/chosen` (initrd start and end as 64-bit values, `stdout-path`, no `bootargs`), `/cpus` (`timebase-frequency` 10 000 000, cell widths 1 and 0, hart `reg` 0), and `/soc/serial` (`ns16550a`, `reg` 0x10000000, 0x100).
- The malformed cases are tested to bite: with each check below removed in turn, the test failed. Caught: `totalsize` larger than the buffer, `End` not the last token, depth bound, a second root, a missing reservation terminator, an unterminated property name, an embedded NUL in a string, subtree skipping that ignores nesting, the unit-address boundary in a name match, the structure block's and the reservation block's alignment. Two removals were *not* caught, and they are equivalent: the check on a value's length is repeated by the check on the aligned end of the value (it matters only where `usize` is 32 bits), and the alignment check on a token offset is on offsets only `item_at()` itself produces.
- The first run of the alignment tests did not catch their mutants: the block that was supposed to be only misaligned was also garbage, so another check refused it with the same status. They now build a valid block at an odd offset (`Builder::structure_gap`) so only the alignment rule can refuse it.
- clang-format clean; clang-tidy (`-p out/bringup.x86`, header filter on `kernel/phys/lib/fdt`) reports nothing.
- `ours-verify`: build ok, dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), 148 host tests pass (the two new ones).
- Not measured: compiling it in the riscv phys toolchain (the shim does not include it yet), a tree from real riscv hardware, and the tree OpenSBI passes on, which differs from the dumped one (OpenSBI adds a `/reserved-memory` node for its own region).

**Deliberately not done**
- Wiring it into the shim. The next step reads the tree there and prints the memory ranges and the initrd range.
- Phandles, aliases, `ranges` translation, `interrupt-map`: nothing in the boot path needs them.
- `/reserved-memory` handling. The reservation block is exposed; the node is just a node here, and what to do with it is the shim's decision.
- Cell widths above two (64 bits) and version 16 blobs. Both are refused with `NotSupported`.
- A string-list accessor for `compatible`. Add one when a caller has to match on it.

**How it was found**
None, a new feature.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does `Property::reg()` need the cell widths passed in, and whose properties are they?
- A property is listed after a child in a blob. Why does `Node::property()` not see it, and why is that the right behaviour for a reader of this format?
- `Tree::from()` already walked the whole structure block. Why do the accessors still bounds-check every read?
