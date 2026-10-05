# unwind changes

## PC-relative pointers in .eh_frame are measured from where the section is, not from its start (2026-10-05)

**Files**
- `kernel/lib/unwind/include/unwind/cfi.hpp` — `Section::address`: the address of the section's first byte in the address space its PCs belong to, with what it means for a section read in place and for a copy
- `kernel/lib/unwind/src/cfi.cpp` — `parse_fde()` anchors `pc_begin`, and `parse_cie()` the personality pointer, at `section.address` plus the field's offset
- `kernel/lib/unwind/include/unwind/cursor.hpp` — `encoded_pointer()`'s `pc_base` is said to be the field's address
- `kernel/kernel/sampler/sampler.cpp` — the kernel's `.eh_frame` is read in place, so its address is `__eh_frame_start`
- `kernel/lib/unwind/test/cfi_test.cpp`, `kernel/lib/unwind/test/unwind_test.cpp` — the builders take the address their buffer pretends to be at (0 unless a test says otherwise); two new tests read a section loaded high up, as the kernel's is
- `docs/repairs/261004/prof.md` — its one item, kernel samples one frame deep, is fixed, so the file goes

**Symptom**
Every kernel sample was one frame deep: `prof-fold` showed leaf functions only, with no callers, so a flame graph was flat (`docs/repairs/261004/prof.md` item 1, found 2026-10-04). Each record had depth 1 and no flags, which is what `Sampler::unwind_context()` writes when the very first `Unwinder::step()` answers `NoFde`: the index had no entry for the interrupted PC.

**Cause**
`.eh_frame` stores an FDE's start address in the encoding its CIE names, and on x86-64 that is `0x1b`: `DW_EH_PE_pcrel | DW_EH_PE_sdata4`, a signed 32-bit value to add to the address of the field holding it. That is what lets a position-independent image (the kernel is linked at 0 and runs at `0xffffff8840000000`) be placed anywhere without relocating its unwind tables: field and function move together, so their distance stays.

"The address of the field" means its address in the address space the code runs in. `parse_fde()` used its offset within the section instead:

```cpp
std::uint64_t const pc_base = offset + cursor.offset();   // offset in the section
fde.pc_begin = cursor.encoded_pointer(cie.fde_encoding, pc_base);
```

So every start address came out short by the section's own address: function address minus `.eh_frame`'s address, a value near `2^64` here, which no PC is. Every lookup missed. The same mistake sat in the personality pointer of a CIE's `P` augmentation.

The unwinder's tests did not catch it because they shared the mistake. Their builders wrote each field as `pc - field_offset`, which is the correct encoding only for a section at address 0, and at address 0 an offset and an address are the same number. A test that builds its expectations from the code's own model checks that the code is consistent with itself, not that the model is right; the new tests put the section where the kernel's is.

`Section` had no way to say where its bytes are, because it was written for the one case where you need not ask: bytes read in place, whose address is the pointer. Giving it an `address` covers that case (the kernel passes `__eh_frame_start`) and the other one, a copy read from a file or built in a test, whose bytes live somewhere other than the address the PCs are relative to.

**Change**
Before:

```cpp
struct Section final {
    std::uint8_t const *data;
    std::size_t size;
    Kind kind;
};
std::uint64_t const pc_base = offset + cursor.offset();
```

After:

```cpp
struct Section final {
    std::uint8_t const *data;
    std::size_t size;
    Kind kind;
    std::uint64_t address;   // where data[0] is in the PCs' address space
};
std::uint64_t const pc_base = section.address + offset + cursor.offset();
```

Rejected: anchoring at `data` itself (`reinterpret_cast<u64>(section.data) + offset`). It is right for the kernel, but it makes a section that is a copy unreadable, and every test would have to encode against its buffer's heap address.

**Reference**
The x86-64 psABI's `.eh_frame` and the `DW_EH_PE_pcrel` application in the Linux Standard Base Core Specification's exception frame section: the value is relative to "the current PC", the address of the encoded value. LLVM's libunwind decodes it in `LocalAddressSpace::getEncodedP()` (`libunwind/src/AddressSpace.hpp`): `startAddr` is the field's own address, and `DW_EH_PE_pcrel` does `result += startAddr`; `CFI_Parser::decodeFDE()` (`DwarfParser.hpp`) calls it for the FDE's pc_begin. Checked in the llvm-project copy vendored with Rust under the Fuchsia checkout's `prebuilt/third_party/rust`.

**Verification**
Host: unwind `cfi` tests 11/11 (two new) and `unwind` tests 5/5; both new tests fail with the old anchor (`ParseFde.PcrelBeginIsMeasuredFromWhereTheSectionIs`, `FdeIndex.FindsPcsOfASectionLoadedHighUp`). Prof tests 22/22. `boot_check.py --out out/sampler`: dbg PASS (31 passed, 2 skipped), rel PASS (27 passed, 9 skipped, 2/2 required). On dbg the `profile` run's 254 kernel records now unwind, up to ten frames deep, and 579 of their return addresses sit right after a `call` instruction in `vmours` (checked by disassembling it and matching each return address against the instruction after each call): `Thread::trampoline` → `labour_routine` → `logz4::vlog` → `SerialLogger::write`, and the `vmo_create` path from the syscall entry down to the slab allocator.

37 frames in that run land outside the kernel image, such as `0xffffff800015f5c0` above a `vmo_create` frame: the next entry fixes the cause.

**Deliberately not done**
None.

**How it was found**
From the symptom to the line: every record had depth 1 and no flags, so the first step answered `NoFde`. `llvm-readelf --unwind` on the same `vmours` showed an FDE covering the sampled PC (`SerialLogger::write`, `0x31ea0`, range `0xe4`), so the index, not the tables, was wrong. `xxd` of the CIE gave augmentation `zR` and R byte `0x1b`, pc-relative. Decoding the first FDE by hand, its raw value `-620488` plus its field's address `0x9fa28` gives `0x8260`, which is what readelf prints, while `parse_fde()` added the field's offset `0x20` instead.

**The user's prediction**
None.

**Three questions you should be able to answer**
- What does `DW_EH_PE_pcrel` add the stored value to, and why does that let an image move without relocating its `.eh_frame`?
- Why did the unwinder's tests pass with the bug, and what kind of test would have caught it?
- Why does `Section` need an address of its own rather than using `data`?

## A CFI program's rows apply up to the PC, counted from the function's first address (2026-10-05)

**Files**
- `kernel/lib/unwind/src/unwinder.cpp` — `evaluate()` takes the location a program starts at; `step()` passes the FDE's `pc_begin`, for the CIE's initial program and the FDE's own
- `kernel/lib/unwind/test/unwind_test.cpp` — `Unwinder.RowsAfterThePcDoNotApply`: a function whose last row is its epilogue's, sampled in its body, and at its `ret`

**Symptom**
With the previous fix the index found every PC, and most folded stacks were right. Some were not: 37 frames in one run were not in the kernel at all, such as `0xffffff800015f5c0` above a `vmo_create` frame.

**Cause**
An FDE's program is a table of rows, each saying how to find the caller's frame from a given address on: "from the first instruction, the CFA is rsp + 8; from after `push rbp`, rsp + 16; from after `mov rbp, rsp`, rbp + 16; from the epilogue, rsp + 8 again". The rows are written as advances, small deltas, from the function's first address. To find the rule for a PC you run the program until the next advance would pass the PC.

`evaluate()` counted the advances from 0 and compared the result with the real PC:

```cpp
std::uint64_t location = 0;                   // deltas from 0 ...
location += delta * cie.code_align;
if (location > target) { return true; }       // ... against an absolute PC
```

For a kernel PC the location never got close, so every row of the function applied, and the rule used was the last row's. For a function with one return at its end that is the epilogue's rule, CFA = rsp + 8, which is wrong anywhere in the body: the unwinder read the caller's return address from the top of the body's stack, where a local sits, and walked off into memory.

It went unnoticed for two reasons. While the index found nothing (the previous entry), no row was ever evaluated in the kernel. And the functions that dominate the dbg image's samples have a return in the middle: `SerialLogger::write()` returns from inside, and its program ends with a row putting the body's rule back (`DW_CFA_def_cfa: rbp + 16` after the `ret`), so "every row" landed on the right rule for it by luck. The unwinder's tests used small PCs from 0x1000 up and functions with no row after the sampled PC, where every row applying and the right rows applying agree.

**Change**
`evaluate()` gains the starting location, and `step()` gives it the FDE's first address:

```cpp
auto evaluate(Section const &section, std::uint64_t begin, std::uint64_t end,
              std::uint64_t start, std::uint64_t target, Cie const &cie, EvalState *state) -> bool;
...
std::uint64_t location = start;
...
evaluate(section, fde.instructions_begin, fde.instructions_end, fde.pc_begin, regs->pc, cie, &state);
```

A row whose location equals the PC still applies (`location > target` stops only past it), which the new test checks at the `ret`.

Rejected: passing `regs->pc - fde.pc_begin` as the target instead. It works for advances but not for `DW_CFA_set_loc`, whose operand is an address, not a delta.

**Reference**
DWARF 5, section 6.4.1: each row's location is the FDE's initial location advanced by the program's `DW_CFA_advance_loc` deltas, and a PC is governed by the last row at or before it. LLVM's libunwind `CFI_Parser::parseFDEInstructions()` (`DwarfParser.hpp`) starts its `codeOffset` at 0 and compares it with `upToPC - pcStart`, the other consistent choice.

**Verification**
Host: unwind tests 6/6 (one new) and cfi 11/11; the new test fails without the fix (the caller's PC comes back as `0xDEADBEEF`, the word the test put at the top of the body's stack). `boot_check.py --out out/sampler`: dbg PASS (31 passed, 2 skipped), rel PASS (27 passed, 9 skipped, 2/2 required). On dbg, every one of the 761 return addresses in the `profile` run's 255 kernel records sits right after a `call` instruction in `vmours`, and none is outside the kernel image, where the same check found 37 after the previous fix alone.

**Deliberately not done**
None.

**How it was found**
The frame outside the kernel image in the previous entry's run. Reading `step()`, the FDE program got the absolute PC as its target while `evaluate()` started counting at 0. Dumping `SerialLogger::write()`'s program with `llvm-readelf --unwind` showed why most stacks still looked right: its last row, after its mid-function `ret`, restores the body's rule.

**The user's prediction**
None.

**Three questions you should be able to answer**
- How does an FDE's program say which rule holds at a given PC, and where does its location start?
- Why did most stacks come out right while this bug was live, and which kind of function exposed it?
- Why not just subtract `pc_begin` from the PC instead of starting the location at `pc_begin`?

## A frame pointer no CFI rule mentions is the caller's, unchanged (2026-10-05)

**Files**
- `kernel/lib/unwind/src/unwinder.cpp` — `step()` starts rbp's rule as "same value" before the CIE's program runs; an explicit `DW_CFA_undefined` still makes it unrecoverable
- `kernel/lib/unwind/test/unwind_test.cpp` — `Unwinder.AFrameThatNeverSavesRbpLeavesItForItsCaller`: a leaf without a frame pointer under a caller with one; `WalksCallChainToLeafAndStopsWithoutFde` now expects the rbp a frame that never touches it hands on, not 0

**Symptom**
After the previous two fixes every return address was right, but some stacks stopped after two frames with `RecordTruncated` set, for example `Mutex::assert_held()` ← `VmObjectPaged::VmObjectPaged()` and `ProcessDispatcher::current()` ← `syscall_dispatch`, where the caller obviously had callers of its own.

**Cause**
After working out the caller's CFA, `step()` recovers the caller's registers from the rules, and for rbp it did this:

```cpp
case RuleKind::Undefined:
    // The caller does not keep a frame pointer we can recover; ...
    new_bp = 0;
```

Undefined is also the rule every register starts with when the CFI says nothing about it. For rbp that is the common case, not the rare one: a leaf built without a frame pointer (`-momit-leaf-frame-pointer`) never touches rbp, so its CFI never mentions it. And rbp is callee-saved: a function that changes it must save it first and say where, so one whose CFI says nothing has left it alone, and the caller's rbp is the value in the register now. Unwinders treat an unmentioned callee-saved register as "same value" for exactly this reason. Setting it to 0 instead broke the very next step: the caller's CFA is rbp + 16, rbp was 0, the read at 8 failed, and the walk stopped one frame above the leaf.

DWARF's own word for "this register cannot be recovered" is `DW_CFA_undefined`, which a program states explicitly. Starting rbp as Same and leaving Undefined to that instruction keeps both meanings apart.

**Change**
Before running the CIE's initial program:

```cpp
EvalState state = {};
state.rules[Registers::RBP] = Rule{RuleKind::Same, 0, 0};
```

The test that walked A → B → C expected B, which never touches rbp, to hand C an rbp of 0, the old behaviour; it now expects the rbp A restored.

Rejected: treating Undefined as Same for rbp wherever it occurs. That would also ignore an explicit `DW_CFA_undefined rbp`, which does mean the value is gone.

**Reference**
DWARF 5, section 6.4.1, the "same value" and "undefined" register rules, and its note that an ABI may give columns defaults other than undefined. LLVM's libunwind `DwarfInstructions::stepWithDwarf()` (`DwarfInstructions.hpp`) starts from `R newRegisters = registers;` and overwrites only the registers whose saved location is not `kRegisterUnused`, which makes every unmentioned register same-value.

**Verification**
Host: unwind tests 7/7 (one new, one expectation changed) and cfi 11/11; without the fix both `AFrameThatNeverSavesRbpLeavesItForItsCaller` and the changed `WalksCallChainToLeafAndStopsWithoutFde` fail. `boot_check.py --out out/sampler`: dbg PASS (31 passed, 2 skipped), rel PASS (27 passed, 9 skipped, 2/2 required). On dbg, all 786 return addresses in the `profile` run's 227 kernel records sit right after a `call`, none outside the kernel image. The short stacks are gone: the program's own kernel stacks run 3 to 11 frames down to the syscall entry, and a secondary CPU's idle thread unwinds to `start_nonboot_cpu` (4 frames, where it had stopped at 2).

**Deliberately not done**
- The outermost frame of a kernel thread (`Thread::trampoline`) is still recorded with `RecordTruncated`: the step past it reads a return address of 0 and answers `BadFrame` instead of reporting the end of the stack. The frames are right; only the flag is. Next change.
- A caller frame is looked up at its return address, not one byte before it, so a call that ends a function (`start_nonboot_cpu` calling `become_idle`) leaves the return address outside the caller's FDE and the walk stops there. Next change.
- CPU 0's idle thread is sampled as its pc alone: its thread records a stack it never runs on (`docs/repairs/261005/kernel-task.md` item 1), so the unwind window refuses it.

**How it was found**
Sorting the kernel records by how they ended: the syscall stacks ended cleanly at `x86_syscall`, which has no FDE, while short ones ended `RecordTruncated` above a leaf. The leaves had no frame pointer, so the failing step was the first one that needed rbp; `step()` turned rbp's missing rule into 0.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is "the CFI does not mention rbp" a statement that rbp is unchanged, not that it is lost?
- Which step failed when rbp came back as 0, and why that one?
- What is the difference between the "same value" and "undefined" rules, and which instruction produces the second?

## A caller's FDE and rules are looked up one byte before its return address (2026-10-05)

**Files**
- `kernel/lib/unwind/include/unwind/registers.hpp` — `Registers::pc_is_return_address`
- `kernel/lib/unwind/src/unwinder.cpp` — `step()` looks up the FDE and evaluates the rows at `pc - 1` for a return address, and marks the pc it hands back as one
- `kernel/lib/prof/cli/main.cpp` — `prof-fold` names every frame but the first at `ip - 1`
- `kernel/lib/unwind/test/unwind_test.cpp` — `Unwinder.ACallThatEndsAFunctionUnwindsWithThatFunctionsRules`

**Symptom**
Found by the review of the three fixes above. A secondary CPU's idle stack stopped at `start_nonboot_cpu`, one frame short of its entry, without being marked a prefix, and the same happened to every walk through a function whose last instruction is a call: 353 of the kernel's 1474 FDEs end in one. In 34 of them the next function's FDE starts exactly at the return address, so the caller was unwound with the next function's rules and recorded a bogus frame; today those are on panic paths only.

**Cause**
Only the first frame's pc is an instruction that was executing. Every pc after it is a return address: the address after a `call`. When the call is the last instruction of a function, the return address is one past the function's end, and that is either nowhere (`NoFde`, and the walk ends quietly) or the next function. The rules that describe the caller at that point, and the function it is in, are those of the call instruction itself, one byte back, which is always inside the caller. The same goes for row selection inside a function: a row that starts exactly at a return address describes the instruction after the call, not the call.

**Change**
`Registers` says which kind of pc it holds, and `step()` uses it:

```cpp
std::uint64_t const pc = regs->pc - static_cast<std::uint64_t>(regs->pc_is_return_address);
if (!locate(pc, &ref)) { return StepResult::NoFde; }
...
evaluate(section, fde.instructions_begin, fde.instructions_end, fde.pc_begin, pc, cie, &state);
...
regs->pc = new_pc;
regs->pc_is_return_address = true;
```

The record keeps the real return address; `prof-fold` symbolises it at `ip - 1`, so a call that ends a function is named after the caller, not the next function.

Rejected: subtracting one from the return address before storing it. The record would no longer say where execution resumes, and every reader would have to know the record had been adjusted.

**Reference**
DWARF 5, section 6.4.4 ("Call Frame Calling Address"): the return address may lie past the end of the calling subroutine, and a consumer looks up the caller one byte before it on x86. libgcc's `uw_frame_state_for()` looks up `_Unwind_Find_FDE(ra - 1)` unless the frame is a signal frame, and LLVM's libunwind `UnwindCursor::setInfoBasedOnIPRegister(bool isReturnAddress)` decrements the pc when it is a return address.

**Verification**
Host: unwind tests 8/8 (one new, failing without the change), cfi 11/11. `boot_check.py --out out/sampler`: dbg PASS (31 passed, 2 skipped), rel PASS (27 passed, 9 skipped, 2/2 required). On dbg, all 1013 return addresses in 253 kernel records sit right after a `call`; a secondary CPU's idle thread now unwinds `ArchIdleThread::idle` → `IdleThread::run` → `become_idle` → `start_nonboot_cpu` → `x86_start_nonboot_cpu`, one frame further than before.

**Deliberately not done**
- Signal frames (the `S` augmentation), whose pc is exact although it came from a stack: the kernel's `.eh_frame` has none.

**How it was found**
The review measured it on the dev.dbg_1 kernel: the FDEs that end in a call, and the 34 followed directly by another FDE.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is every pc after the first frame's a return address, and where does a return address point when the call ends its function?
- What goes wrong with the FDE lookup in that case, and what with the row selection?
- Why keep the real return address in the record and subtract only when looking it up or naming it?
