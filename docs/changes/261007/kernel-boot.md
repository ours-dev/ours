# kernel-boot changes

## Title: the kernel's three yes/no boot options share one Flags word in place of three bools (2026-10-07)

**Files**
- `kernel/include/ours/boot/options.hpp` — `bool m_shell`, `m_debug_read` and `m_panic_reboot` replaced by `Flags m_flags`, a private `enum class Flags { None, Shell, DebugRead, PanicReboot }`; the three accessors test their bit; a private `apply_flag()` declared
- `kernel/top/boot_options.cpp` — the anonymous-namespace `parse_flag(..., bool &result)` became the member `apply_flag(..., Flags flag)`, which sets or clears one bit

**Symptom**
Latent, cannot be triggered today. `ours::boot::BootOptions` kept three yes/no facts as three `bool` members, against the rule of 2026-10-03 (modern-cpp-development) that an object's yes/no facts share one Flags word.

**Cause**
Three `bool`s are three memory locations, and the rule asks for one word when the facts share a protection. These do: `apply()` writes all three, once, from `setup_options()` on the boot CPU, before `INSTALLED` is published and anything reads them; after that the object is read-only. So a single word is safe, and it is what the rule asks for. Nothing about the parse changes: the bare key still means true, `key=<value>` still takes `CommandLineView::parse_bool()`'s grammar, and a value outside it still keeps the current setting and logs it.

The parser had to move into the class. It took a `bool &` to write through; a bit in a word has no address, and `Flags` is private, so a free function in the `.cpp` could not name it. A private member, `apply_flag()`, sets the bit for true and clears it for false.

**Change**
Before:

```cpp
    parse_flag(cmdline, SHELL_OPTION, m_shell);
    parse_flag(cmdline, DEBUG_READ_OPTION, m_debug_read);
    parse_flag(cmdline, PANIC_REBOOT_OPTION, m_panic_reboot);
    ...
  private:
    bool m_shell{false};
    bool m_debug_read{false};
    bool m_panic_reboot{false};
```

After:

```cpp
    apply_flag(cmdline, SHELL_OPTION, Flags::Shell);
    apply_flag(cmdline, DEBUG_READ_OPTION, Flags::DebugRead);
    apply_flag(cmdline, PANIC_REBOOT_OPTION, Flags::PanicReboot);
    ...
  private:
    enum class Flags : u8 {
        None,
        Shell = 0_bit,
        DebugRead = 1_bit,
        PanicReboot = 2_bit,
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(Flags);

    Flags m_flags = Flags::None;
```

and `shell()` returns `!!(m_flags & Flags::Shell)`, the other two likewise.

**Reference**
None, this project's own rule. Zircon's `BootOptions` is generated from `kernel/lib/boot-options/include/lib/boot-options/options.inc`, where each boolean option, `DEFINE_OPTION("aslr.disable", bool, aslr_disabled, ...)` among them, is a `bool` field of its own.

**Verification**
- The build of `out/bringup.x86` is clean; `clang-tidy` reports nothing in `boot_options.cpp` or the header.
- `boot_check.py`: dbg PASS (33 passed, 3 skipped, 0 failed), rel PASS (31 passed, 9 skipped, 6/6 required, `pc-ps2: key a` and `key A` present). Both values of each path are exercised: the dbg image starts the kernel shell (`kernel.shell`, set), the rel image logs `(kshell) not started (kernel.shell is off)` for `kernel.shell=false` (cleared), and the rel user shell reads COM1 through `kernel.debug_read=true`.

**Deliberately not done**
`kernel.panic.reboot` is set by no image, so its bit is only read, never set, in these boots.

**How it was found**
`docs/repairs/261006/cpp-style.md` item 1.

**The user's prediction**
None.

**Three questions you should be able to answer**
- What lets the three options share one word without a lock or an atomic?
- Why did the parser have to become a member function?
- What does `kernel.shell=maybe` do now, and where is that decided?
