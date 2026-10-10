/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_BOOT_OPTIONS_HPP
#define OURS_BOOT_OPTIONS_HPP 1

#include <string_view>

#include <obi/command_line_view.hpp>
#include <ustl/util/enum_bits.hpp>

namespace ours::boot {
/// The boot options vmours itself consumes, parsed once from the image's
/// command line and read from there by whichever subsystem each one steers.
///
/// One object, one parse, and each option's name, default and grammar stated in
/// exactly one place: the kernel-side half of the boot options, with
/// phys::BootOptions the other half (see below). Before it, every consumer did
/// its own `boot_package()` -> `command_line()` -> `.value(key)` walk, so each
/// one also chose its own default, its own length limit and its own reaction to
/// a malformed value -- and a second consumer would have been a second copy of
/// all three.
///
/// The phys stages have their own object, phys::BootOptions, and the split
/// is by consumer rather than by option: those options are spent before the
/// kernel is loaded. The command line itself is not handed across -- the
/// phys handoff deliberately carries none, because vmours can re-derive it
/// from the boot package -- which is why this parses the OBI Config item
/// again rather than receiving a parsed copy from phys. It never sees the
/// bootloader's command line: GRUB's steers phys alone, and what reaches vmours
/// is what the image baked into its Config item.
class BootOptions final {
  public:
    /// Which build of the userboot program the first user process runs, as a
    /// file in the kernel package; see userboot() for what it does and does
    /// not choose.
    static constexpr std::string_view USERBOOT_OPTION{"kernel.select.userboot"};

    /// Whether the kernel shell starts; see shell().
    static constexpr std::string_view SHELL_OPTION{"kernel.shell"};

    /// Whether userspace may read from the serial debug console.
    static constexpr std::string_view DEBUG_READ_OPTION{"kernel.debug_read"};

    /// The kernel's own directory in the kernel package, by the option phys
    /// boots it by; see kernel_name().
    static constexpr std::string_view KERNEL_NAME_OPTION{"kernel.name"};

    /// Whether a panic resets the machine once its report is out; see
    /// panic_reboot().
    static constexpr std::string_view PANIC_REBOOT_OPTION{"kernel.panic.reboot"};

    /// How long a CPU may go without a tick before the lockup detector
    /// reports it; see lockup_threshold().
    static constexpr std::string_view LOCKUP_THRESHOLD_OPTION{"kernel.lockup.threshold"};

    /// lockup_threshold() when the option is absent.
    static constexpr u32 DEFAULT_LOCKUP_THRESHOLD{10};

    /// Whether each CPU's tick device runs one-shot from the moment its
    /// timer device comes up; see time_oneshot().
    static constexpr std::string_view TIME_ONESHOT_OPTION{"kernel.time.oneshot"};

    /// Applies the options `cmdline` names and leaves the rest at their
    /// defaults. A value an option cannot take is logged and ignored rather
    /// than refused: nothing downstream could act on an error, and the
    /// default is always a kernel that boots.
    auto apply(obi::CommandLineView const &cmdline) -> void;

    /// `kernel.select.userboot=<name>`; "userboot" when absent.
    ///
    /// The first user process is always a userboot program; this option
    /// picks only *which* one. Every image here leaves it at the default; it
    /// exists to name another build of userboot, such as one built to run
    /// tests, and never a system service. What userboot starts next is a
    /// separate option, `userboot.next`, which the kernel does not parse: it
    /// forwards every `userboot.*` token to the userboot program as a
    /// processargs environment string (collect_user_options() in
    /// //kernel/lib/userabi/src/userboot.cpp), and the program reads it
    /// (//kernel/lib/userabi/userboot/userboot.cpp).
    ///
    /// The images used to name init or the shell here, so that the program
    /// ran in userboot's place. That left nowhere for userboot's own work --
    /// choosing the next program and handing it the system's handles -- and
    /// the images now say `userboot.next=` instead
    /// (//kernel/products/image/dev/BUILD.gn). Before this option existed at
    /// all there was a compiled-in preference -- start "shell", fall back to
    /// "userboot" when the package has none -- which put an //examples
    /// target's name inside the kernel and made "what ran" a function of what
    /// a package happened to contain.
    ///
    /// Phys does not resolve this option: the phys handoff carries no
    /// userboot image, so userabi::userboot_init() looks the name up in the
    /// kernel package itself.
    ///
    /// The default is "userboot", not "userboot/image.elf":
    /// `kernel_elf_binary("userboot")` in //kernel/lib/userabi/userboot/
    /// BUILD.gn sets `has_patches = false`, and kernel_elf_image() installs a
    /// patch-less binary as one bare file rather than as a `<name>/` subtree
    /// -- which is why vmours, which does have patches, is "vmours/image.elf"
    /// instead. The two live in the same BootFS with different shapes, so the
    /// name cannot be derived from the kernel's own; it has to be stated.
    [[nodiscard]]
    auto userboot() const -> std::string_view {
        return m_userboot;
    }

    /// `kernel.shell`, `kernel.shell=true`: start the kernel shell on COM1.
    /// False when absent.
    ///
    /// Off by default because of what the shell is: a ring-0 thread that runs
    /// whatever registered command a serial line names, with no notion of who
    /// is typing. That is the right thing for a bring-up image driven over its
    /// console and the wrong thing to get by forgetting to say otherwise, so an
    /// image that wants it asks for it (//kernel/products/image/dev/BUILD.gn).
    /// This only decides whether the shell *starts*; whether it is in the
    /// kernel at all is the build argument kernel_shell_enabled
    /// (//kernel/kernel_params.gni), and with that off this option is read and
    /// has nothing to steer.
    [[nodiscard]]
    auto shell() const -> bool {
        return !!(m_flags & Flags::Shell);
    }

    /// `kernel.debug_read`, `kernel.debug_read=true`: permit debug_read when
    /// the caller also holds the System Debug resource. False when absent.
    [[nodiscard]]
    auto debug_read() const -> bool {
        return !!(m_flags & Flags::DebugRead);
    }

    /// `kernel.name=<dir>`; "vmours" when absent -- phys::BootOptions'
    /// default, since both must name the same directory.
    ///
    /// The directory of the kernel package that holds this kernel's own
    /// files, under KernelPackage's fixed names (IMAGE_ELF, CODE_PATCHES).
    /// phys reads the option to find the image it loads (physboot_init()).
    ///
    /// vmours has no reader of it now. Its one use was to keep the kernel's
    /// own files out of the BootFS userabi built for user space, and the
    /// kernel builds no BootFS since userboot unpacks the image's
    /// StorageBootFs item itself (.agents/tasks/userboot-bootfs). A new use
    /// must also know that this copy can disagree with phys's: this parses the
    /// image's Config item again and never sees the bootloader's command line
    /// (see the class comment), so `kernel.name=` typed at GRUB boots a
    /// directory other than the one named here. Such a use should take the
    /// directory from the handoff instead.
    [[nodiscard]]
    auto kernel_name() const -> std::string_view {
        return m_kernel_name;
    }

    /// `kernel.panic.reboot`, `kernel.panic.reboot=true`: after a panic has
    /// printed its report and left its crash record, reset the machine
    /// rather than halt. False when absent.
    ///
    /// Off by default, so that a panic stays on the screen for whoever is
    /// watching, and a kernel that panics on every boot does not reboot in a
    /// loop. The crash record exists for the case where it is on: the next boot
    /// reads it (.agents/tasks/panic-crashlog/DESIGN.md).
    [[nodiscard]]
    auto panic_reboot() const -> bool {
        return !!(m_flags & Flags::PanicReboot);
    }

    /// `kernel.lockup.threshold=<seconds>`: a CPU that takes no tick for
    /// this many seconds is stuck with interrupts off, and the lockup
    /// detector reports every CPU's state and panics
    /// (//kernel/lib/lockup_detector). 0 turns the detector's checks off;
    /// an NMI still asks it for a dump. DEFAULT_LOCKUP_THRESHOLD when absent.
    ///
    /// The seconds are the checking CPU's: each CPU checks the others once
    /// per second of its own ticks, and a CPU is stuck once its heartbeat has
    /// not moved for this many checks in a row. A pause of the whole machine
    /// therefore does not count towards it.
    [[nodiscard]]
    auto lockup_threshold() const -> u32 {
        return m_lockup_threshold;
    }

    /// `kernel.time.oneshot`, `kernel.time.oneshot=true`: a CPU whose timer
    /// device can be armed for one instant uses it that way from the moment it
    /// comes up. The device then interrupts at the earliest deadline in that
    /// CPU's timer queue, so a timer fires near its own deadline, and the tick
    /// is a timer in the same queue. False when absent: the device fires every
    /// 1/HZ, and a timer waits for the next tick.
    ///
    /// Off by default while the one-shot path settles
    /// (.agents/tasks/high-res-timers/DESIGN.md); an image turns it on through
    /// the build argument time_oneshot (//kernel/kernel_params.gni).
    [[nodiscard]]
    auto time_oneshot() const -> bool {
        return !!(m_flags & Flags::TimeOneshot);
    }

  private:
    /// The yes/no options, as one word. Written only by apply(), which
    /// setup_options() runs once on the boot CPU before anything reads them;
    /// read-only after.
    enum class Flags : u8 {
        None,
        Shell = 0_bit,
        DebugRead = 1_bit,
        PanicReboot = 2_bit,
        TimeOneshot = 3_bit,
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(Flags);

    /// Applies the boolean option |option| to |flag| in m_flags: the bare key
    /// sets it, `key=<value>` takes CommandLineView::parse_bool()'s grammar,
    /// and a value outside it keeps what was there.
    auto apply_flag(obi::CommandLineView const &cmdline, std::string_view option, Flags flag)
        -> void;

    Flags m_flags = Flags::None;

    /// A copy, NUL-terminated, rather than a view into the command line: the
    /// value is a slice of the Config item that ends at a space, and holding
    /// the slice would tie this object to the package mapping for good.
    /// 64 bytes is what phys::BootOptions keeps for `kernel.phys.next` and
    /// `kernel.name`.
    char m_userboot[64]{"userboot"};

    /// Same size as phys::BootOptions' copy of this option, so any name phys
    /// accepts fits here too.
    char m_kernel_name[64]{"vmours"};

    u32 m_lockup_threshold{DEFAULT_LOCKUP_THRESHOLD};
};

/// Parses the image's command line into the options options() returns.
///
/// Runs once, right after setup_handoff(): the command line is an item of
/// the boot package, and the handoff is what locates the package.
auto setup_options() -> void;

/// The options this kernel was booted with. Panics before setup_options()
/// has run, the way handoff() does before setup_handoff(): an early reader
/// would otherwise get the defaults and look exactly like a boot that asked
/// for them.
[[nodiscard]]
auto options() -> BootOptions const &;

/// The same, or null before setup_options() has run. For the one reader that
/// must not panic over their absence: the panic path itself, which would
/// otherwise turn an early panic into a second one.
[[nodiscard]]
auto try_options() -> BootOptions const *;
} // namespace ours::boot

#endif // OURS_BOOT_OPTIONS_HPP
