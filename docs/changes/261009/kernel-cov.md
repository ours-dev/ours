# kernel-cov changes

## The kernel reports its own coverage: a kernel variant, a `cov dump` command and a host script (2026-10-09)

**Files**
- `build/config/variants/BUILD.gn`, `build/config/ours/variants.gni` — the `coverage-kernel` config and registry entry, tagged `kernel-only`
- `kernel/products/image/dev.dbg_1/BUILD.gn` — the image's `vmours` label ends with `${ours_variant_suffix}`
- `kernel/kernel_executable.gni` — a kernel built in a toolchain with the `coverage` tag links `//system/lib/profdata:runtime`
- `kernel/kernel.ld` — the seven coverage sections are given places, and bounds, explicitly
- `kernel/commands/cov.cpp`, `kernel/commands/BUILD.gn` — the `cov dump` kernel-shell command
- `scripts/cov_kernel.py`, `scripts/test_cov_kernel.py` — boots the instrumented kernel, runs its tests, takes the dump apart and reports
- `.agents/tasks/cov/DESIGN.md`, `PLAN.md` — the step recorded

**Symptom**
The profile writer existed (`//system/lib/profdata`) and nothing used it: no kernel was built with coverage instrumentation, no kernel linked the writer, nothing got the counters out of a running machine, and nothing turned them into a report. `docs/repairs/261005/prof.md` item 6 is the standing example of what that costs: the kernel halves of the profiler had no automated guard.

**Cause**
Four things stood between the writer and a report, and each has a wrong cheap answer.

Where the counters live. The compiler emits them into a section named `__llvm_prf_cnts`. `kernel/kernel.ld` names the output sections it wants and matches input sections by pattern, and `__llvm_prf_cnts` matches none of them, so the linker places it as an orphan beside whatever has the same flags. In this script that could be inside the RELRO range, which physboot maps read-only once the image is relocated (where the linker would put it was not tried), and then the first instrumented function to run would fault on its own counter. So the sections are placed by hand, the writable ones in the data segment after `.data` and before the RELRO range (in one real link, the shared tree's: counters at `0x156528` to `0x181690`, 176,488 bytes or 22,061 counters; RELRO starts at `0x182000`). Two details of that cost a build each. The output sections must keep the compiler's names: the first version called them `.llvm_prf_cnts`, and `llvm-profdata merge --binary-file=` answered `unable to correlate profile: could not find section (__llvm_prf_cnts)`, because it finds the counters in the unstripped binary by name. And the begin and end symbols are defined in the script (`PROVIDE_HIDDEN(__start___llvm_prf_cnts = .)`) and not left to the linker, because an empty output section is dropped, taking its symbols with it, and the library refers to all seven in every kernel (the plain kernel, before anything linked the library, showed none of them).

How the instrumented kernel gets built. The variant machinery creates a toolchain for each selected variant, but the dbg image names its kernel with a fixed toolchain label, so a selected variant's toolchain was created and never used. `ours_variant_suffix` was defined in `variants.gni` for exactly this and nothing read it. The image's label now ends with it, so `tools/set.py --variant coverage-kernel` builds the dbg image's kernel in `kernel_x86.dbg.1-coverage-kernel`; the rel image keeps `dbg.0` and is not instrumented, since it has no kernel shell to ask for the result. The variant has the `kernel-only` tag, which keeps it out of the host and the phys toolchains, and it carries no linker flag: the kernel links with `ld.lld` directly. It uses binary correlation (`-mllvm -profile-correlate=binary`), so of everything the compiler emits only the counters are loaded into the kernel; the function records, names and coverage map (`__llvm_covdata`, `__llvm_covnames`, `__llvm_covmap`, `__llvm_covfun`) are non-allocated sections that stay in the file, at address 0, and add nothing to what the kernel loads.

How the counters leave the machine. The kernel's console is the only channel every environment has, and it is shared: the debug image's other CPUs log thousands of lines a second, and the kernel shell's prompt and the typed command's echo are written a byte at a time, outside the log's lock, and land in the middle of other lines (`[591793800k0 cpu3 debug]` in a real log). So a plain stream of bytes cannot be trusted, and each line says what it is. The dump is `COV-BEGIN <size> <chunk>`, then `COV <offset> <base64>` lines of 144 bytes each, then `COV-END <size> <FNV-1a checksum>`. The host keeps only lines that match the tag exactly, takes the offsets as the order, requires exactly the line that should be at each offset and no other, and checks the checksum of the whole; a line that was broken fails to parse and is missing, and the dump is asked for again. The first real test was decoding a 105 MB serial log full of other CPUs' lines: 176,672 bytes came back whole.

What happens to a test that kills the machine. `interruptline.test` locked up the machine in the shared tree and `port.test` and `semaphore.test` took it down, which looked like a flaw in this tool until the cause turned out to be another session's uncommitted edit of `Thread::Current::exit()` (not part of this change; in a clean tree all seventeen tests pass). A tool that runs kernel tests has to expect it anyway, so the run dumps after each command: the counters only grow, so the last complete dump holds everything the machine did before it, and a machine that is gone is replaced by a fresh boot that continues with the commands after it, and the profiles of the boots are summed by `llvm-profdata merge`. A command that is found gone is run again first in the next boot, because the machine can be found gone at a command that never ran, the one before it having passed and the lockup coming after; only a command that kills a fresh machine as its very first is called the killer and left out.

The concurrency argument is the one in `docs/changes/261009/profdata.md`: the dump is taken while other CPUs run, each counter is read with a relaxed atomic load, and a counter only grows, so a dump can miss a count but cannot show one that was not reached.

**Change**
The linker script:

```text
    __llvm_prf_cnts : {
        PROVIDE_HIDDEN(__start___llvm_prf_cnts = .);
        KEEP(*(__llvm_prf_cnts))
        PROVIDE_HIDDEN(__stop___llvm_prf_cnts = .);
    }
```

and the same for the other six, the read-only four (`data`, `names`, `vtab`, `vns`) after `.rodata` and the writable three after `.data`. The image: `"//kernel/products/bin:vmours(//build/switch/set/dbg.1:kernel_${target_cpu}.dbg.1${ours_variant_suffix})"`. The command, `cov dump`, builds a `profdata::RawProfile` from the build ID in the image's own `.note.gnu.build-id` (read from the note, not from the absolute symbol the script also defines, which a static PIE is better not to refer to), allocates the dump with `new (GAF_KERNEL) u64[]` (the heap routes large sizes to the virtual allocator), writes the fixed part, copies the counters, and prints the lines. In a kernel built without instrumentation it answers `cov: this kernel was built without coverage instrumentation`.

`scripts/cov_kernel.py` configures `out/cov-kernel` if it does not exist (and refuses a directory configured without the variant), builds, boots the dbg image through `kshell_run.Console`, types every `*.test` command (the names are taken from the `GKTL_COMMAND` registrations in `kernel/commands`, so there is no second list), dumps after each, merges with `llvm-profdata merge --binary-file=<the unstripped kernel>`, and reports with the same summarising code as `cov_host.py`. `--no-tests` takes only what the boot ran, `--command` adds a command, `--html` writes the annotated report.

**Reference**
Zircon `kernel/lib/instrumentation/profile.cc` (`LlvmProfdataVmo()`) hands the kernel's profile to user space as a VMO, and the host side uses the standard LLVM tools. This tree has no such sink to user space, so the console carries it. Linux `kernel/gcov` exports its data through debugfs, from which a tool reads it as files.

**Verification**
All of it in a clean worktree at `f2719f03` plus this change, because the shared tree carries other sessions' uncommitted edits:
- `python3 scripts/cov_kernel.py`: build 20.5 s; all seventeen `*.test` commands print `ok` instrumented; one boot, a 176,696-byte profile; the merge against the unstripped kernel succeeds and the report names 26,894 of 39,491 lines (68.1%), regions 72.0%, branches 54.5%, functions 76.9%. The denominator is the whole kernel: code that never ran is in the profile as zeros, unlike the host report. Some rows, for what they are worth: `kernel/object` 88.0%, `kernel/mm` 75.7%, `kernel/kernel/task/sched` 72.6%, `kernel/lib/lockup_detector` 13.3% (its reports fire only on a lockup), `kernel/lib/crashlog` 12.9%, `kernel/lib/kconcurrent` 0%, and `system/lib/profdata` 95.8%, which the dump exercises on itself.
- `ours-verify`: build ok, dbg 35 passed 0 failed, rel 33 passed 0 failed, host tests 144 passed.
- The 17 decoder tests in `test_cov_kernel.py` pass: a clean dump; short and exact-multiple last chunks; other CPUs' lines between ours; the prompt and echo in front of a line; a line broken in the middle, a missing line, a wrong checksum, a line that ends short and a flipped byte, each refused; the last complete of several dumps wins; the FNV-1a known values.
- A plain kernel boots and answers `cov dump` with the not-instrumented message. `clang-tidy` reports nothing in `cov.cpp`; `clang-format` is clean.
- In the shared tree (not the basis of the numbers above), the same flow showed what a killing test does: the machine was reported gone at `port.test` and `semaphore.test`, rebooted, and the run went on, which is the path the recovery code is for. Only the end state was not seen there.
- Not verified: arm and riscv; the phys modules and user space (not instrumented); the report against an independent measure; `-fcoverage-mcdc` in the kernel; a recovery run to its end.

**Deliberately not done**
- The rel image, the phys modules and user space are not instrumented. Phys runs before any console exists.
- No threshold, no failing on low coverage, no comparison with an earlier run, and no sum of the kernel and host reports per unit.
- `ours_variant_suffix` is set only when exactly one variant is selected by name, which is the case `--variant` makes; a scoped selector does not set it.
- The `*.test` commands that end the machine on purpose (`crash`, `lockup`) are not in the list, since they are not `*.test` commands.
- Three choices were made without stopping to ask, at the user's "just do it": the console as the transport, the build ID read from the image's note, and `cov dump` being part of every dbg kernel.

**How it was found**
The RELRO risk came from reading `kernel.ld` before any build. The name requirement came from running `llvm-profdata merge` on a dump the decoder had just accepted. A first look at the serial log with `grep -c` printed nothing for lines that were there: the log holds control characters, so `grep` treats it as binary and wants `-a`. The machines that did not survive the tests were told apart from this change by running the same test on a plain kernel (it failed the same way), then in a clean worktree (it passed), and finally by the other session that owned the cause.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does `kernel.ld` give the coverage sections places of their own, and why must their output sections keep the compiler's names?
- Why does every line of the dump carry its offset, and what does the checksum at the end add that the offsets do not?
- A test takes the machine down. Why is what the machine ran before it still in the report, and what does the script do about the commands after it?
