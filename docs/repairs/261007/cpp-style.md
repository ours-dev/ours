# cpp-style outstanding repairs

## 1. The bool fields of plain structs were never checked against the Flags-word rule

**Severity**
Low: style and a few bytes of padding per object; nothing behaves wrongly.

**Symptom**
`docs/repairs/261006/cpp-style.md` item 1 searched for `bool m_...` and so reached only class members named with the `m_` prefix. Public fields of plain structs have no prefix, and none was looked at. In headers, outside the third-party trees, these 17 were found: `examples/shell/shell_launcher.hpp` (`Starter::component`), `examples/shell/shell_tests.hpp` (`component`), `kernel/arch/x86/lib/arch/include/arch/x86/apic/ioapic.hpp` (`IoApicIsaOverride::remapped`), `kernel/lib/acpi/test/acpi_test_blob.hpp` (`fail_all`), `kernel/lib/prof/include/prof/sampler.hpp` (`from_uspace`), `kernel/lib/unwind/include/unwind/cfi.hpp` (`augmentation_present`), `kernel/lib/unwind/include/unwind/registers.hpp` (`pc_is_return_address`), `kernel/mm/include/ours/mm/protection_ranges.hpp` (`NextResult::valid`), `kernel/mm/include/ours/mm/vm_cow_pages.hpp` (`RequireResult::writable`), `kernel/phys/include/ours/phys/kernel_package_handoff.hpp` (`PackagePayloadHandoff::present`), `system/lib/eftl/include/eftl/elf/relocations.hpp` (`RelativeRelocation::addend_in_place`), `system/lib/elfload/include/elfload/program.hpp` (`write`, `execute`, `relro`), `system/lib/loop/test/fake_kernel.hpp` (`Object::peer_closed`), `system/lib/ustl/include/ustl/io/binary_reader.hpp` (`valid_`, also misnamed: a class member without `m_`), `system/lib/vfs/include/vfs/tree.hpp` (`executable`). Struct fields in `.cpp` files were not surveyed; a search for them also matches local variables, so each hit needs a look.

**Cause**
The modern-cpp-development rule of 2026-10-03 speaks of "the yes/no facts about one object", which a plain struct also has: `elfload`'s mapping description carries three, `write`, `execute` and `relro`, the shape the rule replaces with one word. But a plain struct is a record its readers fill and read field by field, often with designated initialisers, and some cross a boundary whose layout is fixed: `PackagePayloadHandoff` is part of the phys handoff. Whether the rule reaches these, and whether a handoff struct should change for it, is a decision, not a style fix.

**Change**
To decide with the user first: apply the rule to plain structs, or state in modern-cpp-development that it covers objects with an invariant and not plain records. If it applies, sweep these as item 1 of 261006 was swept, one subsystem per commit, leaving handoff layouts for last.

**Reference**
None, this project's own rule.

**Acceptance**
The rule's scope is written down in modern-cpp-development, and, if it covers plain structs, each field above is derived, in a Flags word, or carries a comment saying why it stays.
