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
