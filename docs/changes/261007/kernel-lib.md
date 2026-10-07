# kernel-lib changes

## Title: kernel/lib's bool members: ccrt's and unwind's derived from what the object holds, kconcurrent's a Flags bit, and the lone ones say why they stay (2026-10-07)

**Files**
- `kernel/lib/ccrt/include/ccrt/runtime.hpp` — `Registry::Record::m_active` removed; `CallbackKind` gains `None`, a free slot
- `kernel/lib/ccrt/src/runtime.cpp` — `compact()` and `matches()` skip `None` records; `register_callback()` no longer sets a flag; `finalize()` retires a record by setting its kind to `None`
- `kernel/lib/unwind/include/unwind/cursor.hpp` — `Cursor::m_ok` removed; `ok()` is `m_data != nullptr` and `fail()` drops the pointer
- `kernel/lib/unwind/src/cursor.cpp` — every `m_ok = false` is `fail()`, every test of it `ok()`
- `kernel/lib/kconcurrent/include/kconcurrent/chain_lock_transaction.hpp` — `bool m_has_mutated` replaced by `Flags m_flags`, a private `enum class Flags { None, Mutating }`
- `kernel/lib/kconcurrent/src/chain_lock_transaction.cpp` — the constructor initialises `m_flags`
- `kernel/lib/crashlog/include/crashlog/crashlog.hpp`, `kernel/lib/acpi/include/acpi/table_set.hpp`, `kernel/lib/unwind/test/unwind_test.cpp` — `Writer::m_truncated`, `EntryRange::m_malformed` and the test fixture's `m_build_ok` say why they stay a `bool`

**Symptom**
Latent, cannot be triggered today. Six `bool` members in kernel/lib against the rule of 2026-10-03 (modern-cpp-development): two restated a fact the object already held, one was a state of its object kept apart from a Flags word, and three had no comment saying why they stay.

**Cause**
The rule asks, of every `bool` member: derive it if it restates a fact the object already holds; otherwise give it a bit of the class's Flags word, starting one when the class has two or more such facts or the fact is a state of the object; keep a lone `bool` only with a comment saying why.

- `ccrt::Registry::Record::m_active` restated its kind. A record is either free or holds a destructor or an exit function, and `m_kind` already named the second two; the flag added "free" beside it, so a record had two fields that had to agree, and a reader of either could not trust it alone. A third kind, `None`, is "free", and it is zero, so the registry's zero-initialised table starts all free exactly as the zero `bool` did.
- `unwind::Cursor::m_ok` restated the data pointer. The constructor already defined it as `data != nullptr`; after that, a failed read set it false and left the pointer. Dropping the pointer on failure makes "no data" the one state that cannot read, whether the cursor was built over none or failed since. Nothing reads `m_data` except to read through it, and `offset()` reads `m_pos`, which failure leaves alone, so no caller sees a difference.
- `kconcurrent::ChainLockTransaction::m_has_mutated` is a state of the transaction, its phase: walking, then mutating. Such a fact gets a Flags word even alone, as an exceptionate's shutdown did, so the next fact the transaction keeps has its place. Its protection is simple: only the owning thread touches it, and interrupts are off for the transaction's whole life.
- `crashlog::Writer::m_truncated`, `acpi::EntryRange::m_malformed` and the unwind test fixture's `m_build_ok` are outcomes reported to a caller, each its object's only yes/no fact, and none can be derived: a payload that exactly fills the region has the same `m_length` as one that was cut, an entry range's extent does not say whether a walk stopped early, and the fixture's index does not say whether its build succeeded. They stay, and their comments now say so.

**Change**
ccrt, before and after:

```cpp
    enum class CallbackKind : Byte {
        Destructor,
        ExitFunction,
    };
    struct Record {
        ...
        CallbackKind m_kind;
        bool m_active;
    };
```

```cpp
    enum class CallbackKind : Byte {
        None,
        Destructor,
        ExitFunction,
    };
    struct Record {
        ...
        CallbackKind m_kind;
    };
```

with `finalize()` writing `candidate.m_kind = CallbackKind::None` where it wrote `candidate.m_active = false`.

The cursor, before and after:

```cpp
    Cursor(std::uint8_t const *data, std::size_t size) noexcept
        : m_data(data), m_size(size), m_pos(0), m_ok(data != nullptr) {}
    auto ok() const noexcept -> bool {
        return m_ok;
    }
    auto fail() noexcept -> void {
        m_ok = false;
    }
```

```cpp
    Cursor(std::uint8_t const *data, std::size_t size) noexcept
        : m_data(data), m_size(size), m_pos(0) {}
    auto ok() const noexcept -> bool {
        return m_data != nullptr;
    }
    auto fail() noexcept -> void {
        m_data = nullptr;
    }
```

The transaction, before and after:

```cpp
    bool m_has_mutated;
```

```cpp
    enum class Flags : u8 {
        None,
        Mutating = 0_bit,
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(Flags);

    Flags m_flags;
```

**Reference**
None, this project's own rule. Zircon's chain-lock transaction keeps the matching fact, that the lock set is complete and no more may be taken, as `bool finalized_` (`system/ulib/concurrent/include/lib/concurrent/chainlock_transaction.h`), a plain `bool`.

**Verification**
- The build of `out/bringup.x86` is clean; `clang-tidy` reports nothing on the changed lines (two pre-existing `modernize-use-nodiscard` in `chain_lock_transaction.hpp`).
- Host tests: `unwind` 10 passed, `cfi` 11 passed, `crashlog` 9 passed, `table-set-test` exit 0.
- `boot_check.py`: dbg PASS (33 passed, 3 skipped, 0 failed), rel PASS (31 passed, 9 skipped, 6/6 required, `pc-ps2: key a` and `key A` present). ccrt's registry runs on every boot, for static initialisers and their destructors.
- Not exercised at run time: nothing in the kernel makes a `ChainLockTransaction` yet, so its change is checked by the build only.

**Deliberately not done**
The ccrt tests in `kernel/lib/ccrt/test/` are not wired into any GN target, so they were not run.

**How it was found**
`docs/repairs/261006/cpp-style.md` item 1.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why did `None` have to be the zero value of `CallbackKind`?
- After `fail()`, what does `offset()` return, and why does that not change with this record?
- The transaction's flag became a Flags word but the crash log writer's stayed a `bool`. What distinguishes them?
