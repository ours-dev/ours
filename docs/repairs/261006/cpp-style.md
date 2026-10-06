# cpp-style outstanding repairs

## 1. 35 bool members are left where the rule asks for a Flags word or for no stored fact at all

**Severity**
Low: style and a few bytes of padding per object; nothing behaves wrongly.

**Symptom**
`git grep -nE '^[[:space:]]+bool[[:space:]]+m_[a-z_]+'`, outside the third-party trees, lists 35 members in these files: kernel/include/ours/boot/options.hpp (3), system/lib/ustl/include/ustl/result.hpp (2), system/lib/ustl/include/ustl/collections/detail/btree.hpp (2), system/lib/eftl/include/eftl/elf/version_binding.hpp (2), kernel/mm/include/ours/mm/vm_area_or_mapping.hpp (2), kernel/include/ours/task/auto_preempt_disabler.hpp (2), system/lib/vfs/src/tree.cpp (1), system/lib/ustl/test/btree_extension_test.cpp (1), system/lib/ustl/include/ustl/option.hpp (1), system/lib/ustl/include/ustl/collections/intrusive/detail/hashtable_node.hpp (1), system/lib/ustl/include/ustl/collections/intrusive/bstree_algorithms.hpp (1), system/lib/logz4/src/log.cpp (1), system/lib/eftl/include/eftl/elf/relocations.hpp (1), kernel/phys/include/ours/phys/elf_image.hpp (1), kernel/phys/include/ours/phys/boot_options.hpp (1), kernel/object/include/ours/object/port_dispatcher.hpp (1), kernel/object/include/ours/object/exceptionate.hpp (1), kernel/object/include/ours/object/channel_dispatcher.hpp (1), kernel/mm/include/ours/mm/pm_zone.hpp (1), kernel/lib/unwind/test/unwind_test.cpp (1), kernel/lib/unwind/include/unwind/cursor.hpp (1), kernel/lib/kconcurrent/include/kconcurrent/chain_lock_transaction.hpp (1), kernel/lib/gktl/include/gktl/defer.hpp (1), kernel/lib/crashlog/include/crashlog/crashlog.hpp (1), kernel/lib/ccrt/include/ccrt/runtime.hpp (1), kernel/lib/arch/include/arch/auto_interrupt_disabler.hpp (1), kernel/include/ours/task/auto_migrate_disabler.hpp (1), kernel/arch/riscv/lib/arch/include/arch/riscv/interrupt.hpp (1).

**Cause**
The modern-cpp-development rule of 2026-10-03 asks for the yes/no facts about one object to share a Flags word. Two `bool`s are two memory locations, each padded to the alignment of the member after it. The rule was written after most of these members, and nothing enforces it: clang-tidy has no check for it.

**Change**
For each class:

1. If the `bool` restates a fact the object already holds, derive it and drop the member, as `VmoDispatcher::is_vdso()` now reads its code segment (docs/changes/261006/object.md).
2. Otherwise put it in the class's Flags word, or start one when the class has two or more such facts. Every bit of one word must be written under the same protection, the same lock or atomic read-modify-writes; a fact guarded differently keeps a word of its own.
3. A lone `bool` with nothing to share a word with and nothing to derive it from may stay. Say why in its comment.

Out of scope:

- RAII scope objects such as `AutoPreemptDisabler`, whose `bool` says whether the object still holds its state;
- `ustl::Option` and `ustl::Result`, whose engaged flag is the type's own representation.

**Reference**
None, this project's own rule.

**Acceptance**
The grep lists only members whose comment says why they stay, the build is clean, and `boot_check.py` passes.
