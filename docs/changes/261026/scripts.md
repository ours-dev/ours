# scripts changes

## Title: code_size.py sizes the code an operation may run through, by its call closure in an ELF; page-walk's stage 2 code size (2026-10-26)

**Files**
- `scripts/code_size.py` — new: an operation's closure from its entry function, by the direct targets in its disassembly, inside the code named by `--within`; samples in `perf_stats.py`'s format
- `scripts/test_code_size.py` — new: the closure on a symbol table and a disassembly made up for the test
- `docs/perf/261025/page-walk.md` — code size in Method, and stage 2's numbers
- `docs/perf/261025/page-walk/stage-2-code-size.csv` — new: stage 2's code size in the rel kernel
- `.agents/tasks/x86-page-table-crtp/PLAN.md` — step 1 done

**Symptom**
None, a new measurement. The CRTP rewrite of x86's page table (`.agents/tasks/x86-page-table-crtp`) is argued for by less code and better use of the instruction cache, and the page-walk report had no number for code: only time on the host and instructions a call.

**Cause**
The size of one function does not say what an operation costs in code. The compiler decides what to inline: `PageTable::unmap_pages()` holds the whole engine walk (1917 bytes), while a map calls an out-of-line `map_with()` that calls a `map_range()` per level. Comparing function sizes across the rewrite would compare the inliner's choices, not the code.

**Change**
An operation is sized by its closure: the entry function and every function reachable from it, breadth first, each counted once by its address (so copies the linker folded into one count once). A function is reached through a direct call, a jump to another function (a tail call), or an address of it the code takes (`leaq ...(%rip)`, `# 0x55540 <phys_to_virt>`), all read from `llvm-objdump`'s `0x... <symbol>` targets:

```python
for target in targets:
    callee = symbols.containing(target)
    if callee is None or callee.address == function.address:
        continue  # data, or a branch inside the function
    if callee.address in seen:
        continue
    if not any(p.search(callee.name) for p in within):
        external.add(callee.name)
        continue
    seen[callee.address] = callee
    order.append(callee)
```

`--within` bounds the code being measured, by demangled name: without it a map's closure would take in the allocator, the lock and the IPI path. Calls through a register cannot be followed and are counted (`indirect`), so a closure that shrank only by losing sight of a callee shows it. The output is one sample a case in the CSV `perf_stats.py` reads, so code size goes through the same statistics and comparison as time.

The llvm tools come from `prebuilt/clang` through `fetch_llvm_coverage.find_compiler()` and `locate_tool()`, as `cov_host.py` finds them.

**Reference**
None, this project's own tool. Bloaty (`google/bloaty`) attributes a binary's bytes to symbols and compile units, but not to the call closure of an entry point.

**Verification**
- `python3 scripts/test_code_size.py`: 4 tests pass. A function that calls nothing is its own size; calls, tail calls and taken addresses are followed once; a branch inside the function, data and callees outside `--within` are not.
- On the rel kernel, the closure's functions and sizes agree with `llvm-nm -S`, and the indirect calls it counts are there in the disassembly: `callq *%rdx` and `callq *-0x38(%rbp)` in `map_range<2>`, two in `map_with()`.
- Stage 2's numbers: a map 8626 bytes in 20 functions with 23 indirect calls; a bulk map 8692; unmap 3232; protect 3129; query 481.

**Deliberately not done**
- Instruction-cache misses: they need a hardware PMU.
- Sizing code on paths actually taken: that is lldb's instruction count, already in the report.

**How it was found**
None: a new tool.

**The user's prediction**
None: homework is paused (2026-10-21). The user's reason for the rewrite was that the engine generates code that means nothing, twice. The measurement bears it out for map: its closure holds the engine twice, once per kind of table (2.9 KB and 4.8 KB with deposits), because `PageTable` instantiates every operation for both.

**Three questions you should be able to answer**
- Why does a single function's size not say what an operation costs in code?
- Why is a map's closure twice the code one kind of table needs, although a call only ever runs one of them?
- Why does the kernel's map make indirect calls that the host bench does not?
