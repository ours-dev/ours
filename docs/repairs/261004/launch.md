# launch outstanding repairs

## 1. Every launch maps the program's whole file into the launcher and unmaps it again, at a cost nobody has measured

**Severity**
Low: a performance cost the design accepted (component-framework D14 b); nothing wrong.

**Symptom**
Not measured. Each `launch::start()` now makes one `vmo_get_size`, one `vma_map` of the whole file into the caller, a page fault for each page of it that the judge, the planner and the binder touch (the mapping is made without MapRange), and one `vma_unmap` with its TLB shootdown. The boot runs four CPUs. Once the Launcher lives in the root (component-framework step 14), every /boot and /svc request waits behind a launch (the design's worst interleaving 1), so this cost lands on every client, not only on the one launching.

**Cause**
D14 (b) checks that a file nobody can write is the only kind parsed by asking `vma_map` for Execute, then reads the file through that mapping (`MappedFile` in `system/lib/launch/src/launch.cpp`). The mapping covers the whole file, while what is read is the ELF header, the program headers, the dynamic section and the tables it points to, and the bytes of each partial page. The map, the faults and the unmap are paid on every launch, also for a program launched many times from the same VMO.

**Change**
Measure first, with the user (CLAUDE.md, performance work): the latency of one launch of fault_probe and of the shell, split into the map, the parse and bind, and the unmap, on one CPU and on four. Candidates:
- Keep it, if the numbers are small next to the snapshots and the relocation writes (items 3 and 4 of `docs/repairs/261002/launch.md`).
- Map with MapRange: no faults, but every page of the file is mapped, read or not.
- D14 (c): `vmo_read` the headers and tables into the launcher's own buffers, as Fuchsia's process_builder does. No mapping and no shootdown, but elfload's Binder needs a range reader instead of the whole file as a span (`system/lib/elfload/include/elfload/binder.hpp:66`). The design names this as the way to go once anything but the BootFS can be executable.

**Reference**
Zircon pays the same per module in its remote dynamic linker: `ld::RemoteDecodedModule::Init()` maps the module through `elfldltl::MappedVmoFile::Init()`, whose destructor unmaps it (`src/lib/elfldltl/mapped-vmo-file.cc:13-55`). Fuchsia's `ProcessBuilder::build()` reads the headers with `Elf64Headers::from_vmo()` instead (`src/lib/process_builder/src/process_builder.rs:437`).

**Acceptance**
The measurement above, before and after, agreed with the user.
