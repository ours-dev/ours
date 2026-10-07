# prof changes

## Dump version 2: each record says its process, and the header counts unmatched ticks (2026-10-07)

**Files**
- `system/public/ours/sampler.hpp` — `SamplerRecord::pid` (records grow from 152 to 160 bytes, ips move from 24 to 32), `SamplerDumpHeader::num_unmatched` (the header grows from 32 to 40 bytes), `SAMPLER_DUMP_VERSION` 2
- `kernel/lib/prof/include/prof/record_buffer.hpp`, `sink.hpp`, `sampler.hpp`, `src/sampler.cpp`, `src/dump.cpp` — a per-buffer skipped count, `Sampler::note_unmatched()`, `SampleContext::pid` stored in the record, the header summing the skips
- `kernel/lib/thread_sampler/src/thread_sampler.cpp`, `BUILD.gn` — the pid of the interrupted thread's process in both contexts; a tick that matches no target is counted
- `kernel/lib/prof/cli/main.cpp` — prof-fold reads versions 1 and 2, puts `pid N;` before the tid, and reports the unmatched ticks
- `kernel/lib/prof/test/format_test.cpp`, `sampler_test.cpp` — the new layout; a test that the pid is recorded and unmatched ticks are counted only while running
- `examples/shell/shell_tests.cpp` — `check_unmatched_ticks()`: a kernel-only session over user arithmetic must count some
- `examples/profile/profile.cpp` — comments: the header and record line lengths
- `.agents/tasks/prof-sampler-object/DESIGN.md`, `RESULT.md` — the open questions move on

**Symptom**
Two things the dump could not say. Which process a user record came from: it had a tid but no process identity, so its addresses could not be matched to a binary, the first need of symbolising user frames. And how many ticks there were in all: a tick that landed where no target was asked for recorded nothing and was not counted, so a user-only profile could not say what share of the time was spent in user code.

**Cause**
The record's only identity was the kernel thread id, and the header had fields only for what was recorded or dropped. Adding either means a layout change, so both wait for one version bump.

**Change**
The record gains the koid of the interrupted thread's process, `user_thread()->process().koid()` on the running thread, 0 for a kernel thread; the thread keeps its own user thread alive, so the read needs no lock even in the interrupt. That needs the object layer's headers in thread_sampler, as the upstream thread sampler has.

An unmatched tick is counted where it happens, on that CPU's buffer, by the same interrupts-off producer that writes its records, so a relaxed counter suffices; `start()`'s reset clears it, and the header sums it with the records and the drops:

```cpp
if (!frame->from_user()) {
    if (kernel target) { record_sample(...); } else { sampler.note_unmatched(cpu); }
} else if (!(user target)) {
    sampler.note_unmatched(cpu);
} else { request the user stack; }
```

prof-fold reads both versions: a version 1 header is the first 32 bytes of today's, and a version 1 record is converted by moving its ips from 24 to 32.

**Reference**
The upstream sampler records a pid and tid with every sample; its profiler resolves each pid's modules by build ID (`src/performance/cpu_profiler/sampler.cc`, `GetProcessModules`).

**Verification**
Host: prof sampler 24/24 (one new), format 4/4, unwind 10/10, cfi 11/11. In a clean worktree at HEAD with only this change (the shared one holds another session's unfinished `VmoChildOptions` change, which fails `vmo_create_child`): a full build and `boot_check.py`: dbg PASS (33 passed, 3 skipped), rel PASS (31 passed, 9 skipped, 6/6 required). On dbg, `shell: kernel-only session counted 6 unmatched ticks`; on dev, `profile: 48 kernel samples, 8 user samples, 0 dropped`. prof-fold on that dev dump prints kernel threads as `pid 0` and profile's samples as `pid 2316;tid 2325;...`, with `0 ticks unmatched` since profile asks for both targets; on a hand-built version 1 dump it folds the record as before, without a pid.

**Deliberately not done**
`num_dropped` stays a u32 that can wrap (repair prof item 1).

**How it was found**
The first step of symbolising user frames, chosen by the user, who also agreed to count the unmatched ticks in the same version bump.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is reading the process koid in the timer interrupt safe without a lock?
- Why can the unmatched count be a relaxed counter on each CPU's buffer?
- How does prof-fold read a version 1 record into today's layout?

## profile says where its code lies, in LLVM's symbolizer markup, before its dump (2026-10-07)

**Files**
- `examples/profile/profile.cpp` — before the dump it prints `PROFP <koid>`, then `{{{reset}}}`, `{{{module:0:profile:elf:<build id>}}}` and one `{{{mmap:...}}}` line per loaded segment, all read from its own headers in memory; it counts the user samples whose pc lies in its own executable segment, and exits 2 when there are none.
- `examples/profile/BUILD.gn` — depends on `//system/lib/eftl:headers` for the ELF header, program header and note view.

**Symptom**
A user record says which process it came from (dump version 2's `pid`) and gives raw addresses, but nothing said which binary those addresses belong to or where it was loaded. prof-fold could only print them as hex: `pid 2322;tid 2331;0x20309b;...;0x202e00 [user]`.

**Cause**
Symbolising a user address needs three facts that only the process itself has at run time:
- Its koid, to match the record's `pid`.
- Its load bias. Programs here are position-independent and the loader picks where each one goes, so `0x202e00` means nothing until the bias (here 0x200000) is subtracted: file address 0x2e00, which is `spin_inner`.
- Its build ID. The binary in BootFS is stripped, so the host has to find the unstripped ELF with the symbols. The 20-byte SHA-1 the linker writes into `.note.gnu.build-id` is the name that matches the two exactly, where a path or a file name could match an older build.

The previous two steps made the koid answerable: `ort::process_self()`, then `object_get_info(HandleBasic)`. This step reads the other two and prints all three.

**Change**
The load bias comes from `__ehdr_start`, a symbol the linker defines at the first byte of the image, the ELF header. Its address is where the image lies. The segment that holds file offset 0 says which program address the header was linked at, and the difference is the bias:

```cpp
auto const image = reinterpret_cast<u64>(&__ehdr_start);
auto const headers = std::span{
    reinterpret_cast<Elf64Phdr const *>(image + __ehdr_start.e_phoff), __ehdr_start.e_phnum};
// the PT_LOAD with p_offset == 0
auto const bias = image - first->p_vaddr;
```

The declaration is hidden (`[[gnu::visibility("hidden")]]`), so the reference is resolved when the program is linked. A default-visibility reference could leave a dynamic symbol, which the loaders' binder cannot bind, since it binds only to the vDSO.

The build ID comes from the PT_NOTE segment, which is loaded with the first segment and read in place with eftl's `NoteSectionView`: the note of type 3 (NT_GNU_BUILD_ID) whose owner is `"GNU\0"`.

The output follows LLVM's symbolizer markup, as the user chose (option A). On dev it reads:

```text
PROFP 2322
{{{reset}}}
{{{module:0:profile:elf:478f67268d12e4840aec533043dad9bb84fd6b3f}}}
{{{mmap:0x200000:0x1000:load:0:r:0x0}}}
{{{mmap:0x201000:0x9000:load:0:rx:0x1000}}}
{{{mmap:0x20a000:0x1000:load:0:rw:0xa000}}}
{{{mmap:0x20b000:0x2b000:load:0:rw:0xb000}}}
```

How to read the markup lines:
- An mmap line is a range in memory, its length, module 0, its rights, and the module-relative address it starts at, in whole pages as the loader maps them.
- A tool resolves an address by finding the mmap that holds it and adding that line's relative address to its offset into the range.
- `PROFP` is this program's own line, saying whose markup follows, because the markup has no process in it.

The program checks its own output: a user record with its own pid whose pc lies in an executable mmap range counts as "in this program", and none at all exits 2. A wrong bias would put every pc outside the ranges, so the shell's `profile checks passed` now also means the markup matches where the samples landed.

Rejected: option B, one ad hoc `PROFM <pid> <base> <build id> <name>` line. It is less code, but only prof-fold would read it. The markup is a published format that `llvm-symbolizer --filter-markup` already reads, and a user-space crash report can print the same lines later.

**Reference**
- LLVM, `llvm/docs/SymbolizerMarkupFormat.rst`: the `reset`, `module` and `mmap` elements, and `llvm-symbolizer --filter-markup`, which turns `{{{bt:...}}}` lines into symbols using them.
- Fuchsia's dynamic linker prints the same module and mmap lines for each module it loads, for crash and sanitizer reports to be symbolised later (`log_dso()` in `zircon/third_party/ulib/musl/ldso/dynlink.c`). This program is one module, so module 0 and no dynamic linker.

**Verification**
- In `.worktrees/wip-proc-self` at 407df9d3 plus this change: `boot_check.py`: `dbg PASS 35 passed, 3 skipped, 0 failed`; `rel PASS 33 passed, 9 skipped, 0 failed, 6/6 required` (profile runs on dev only).
- The printed build ID equals `llvm-readelf -n` on the built profile.
- Each mmap range is its PT_LOAD rounded out to pages, plus a bias of 0x200000.
- `profile: 44 kernel samples, 8 user samples (6 in this program), 0 dropped`. The other two user records were another process's (pid 101), which the session samples too.
- prof-fold reads the same log unchanged: it skips the new lines, and folds profile's user stack as `pid 2322;...;0x202e00 [user]`. Its leaf, 0x202e00 less the bias, is 0x2e00, inside `spin_inner` at 0x2df0 by `llvm-nm`.
- Not checked through `llvm-symbolizer --filter-markup`: the prebuilt LLVM in `prebuilt/clang` does not ship llvm-symbolizer. The address above was resolved by hand instead.

**Deliberately not done**
- prof-fold resolving user frames by build ID: the next step, symbolisation step 3.
- Moving the module reading into ort for other programs, such as a crash report: no second user yet.
- A module line for the vDSO, the one other image mapped into the program: a user sample in vDSO code stays a bare address. The kernel maps the vDSO, so the program would have to find it through `ort::vdso_base()`, which is a separate step.

**How it was found**
None.

**The user's prediction**
None. The user chose option A (LLVM symbolizer markup) over B (one line of this project's own).

**Three questions you should be able to answer**
- Why is `&__ehdr_start` the load address, and why does the declaration have to be hidden in a program the binder links only against the vDSO?
- Why does prof-fold need the build ID rather than the program's path to find the right ELF?
- Given `{{{mmap:0x201000:0x9000:load:0:rx:0x1000}}}`, what file address does the pc `0x202e00` stand for, and how did you get it?
