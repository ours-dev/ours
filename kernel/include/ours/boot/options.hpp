/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_BOOT_OPTIONS_HPP
#define OURS_BOOT_OPTIONS_HPP 1

#include <string_view>

#include <obi/command_line_view.hpp>

namespace ours::boot {
/// The boot options vmours itself consumes, parsed once from the image's
/// command line and read from there by whichever subsystem each one steers.
///
/// This is the kernel-side half of what Zircon keeps in one BootOptions
/// (lib/boot-options, read through gBootOptions): one object, one parse, and
/// each option's name, default and grammar stated in exactly one place.
/// Before it, every consumer did its own `boot_package()` ->
/// `command_line()` -> `.value(key)` walk, so each one also chose its own
/// default, its own length limit and its own reaction to a malformed value --
/// and a second consumer would have been a second copy of all three.
///
/// The phys stages have their own object, phys::BootOptions, and the split
/// is by consumer rather than by option: those options are spent before the
/// kernel is loaded. The command line itself is not handed across -- the
/// phys handoff deliberately carries none, because vmours can re-derive it
/// from the boot package -- which is why this parses the OBI Config item
/// again rather than receiving a parsed copy the way Zircon's kernel receives
/// gBootOptions from physboot. It never sees the bootloader's command line:
/// GRUB's steers phys alone, and what reaches vmours is what the image baked
/// into its Config item.
class BootOptions final {
  public:
    /// Which build of the userboot program the first user process runs, as a
    /// file in the kernel package. Zircon's option of the same name, with the
    /// same default; see userboot() for what it does and does not choose.
    static constexpr std::string_view USERBOOT_OPTION{"kernel.select.userboot"};

    /// Whether the kernel shell starts. Zircon's option of the same name and
    /// the same default; see shell().
    static constexpr std::string_view SHELL_OPTION{"kernel.shell"};

    /// Whether userspace may read from the serial debug console.
    static constexpr std::string_view DEBUG_READ_OPTION{"kernel.debug_read"};

    /// The kernel's own directory in the kernel package, by the option phys
    /// boots it by; see kernel_name().
    static constexpr std::string_view KERNEL_NAME_OPTION{"kernel.name"};

    /// Applies the options `cmdline` names and leaves the rest at their
    /// defaults. A value an option cannot take is logged and ignored rather
    /// than refused: nothing downstream could act on an error, and the
    /// default is always a kernel that boots.
    auto apply(obi::CommandLineView const &cmdline) -> void;

    /// `kernel.select.userboot=<name>`; "userboot" when absent.
    ///
    /// The first user process is always a userboot program; this option
    /// picks only *which* one. Zircon's images leave it at the default and
    /// eng images swap in userboot-test-rust, a userboot built to run tests;
    /// neither puts a system service here. What userboot starts next is a
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
    /// Zircon resolves this option one stage earlier: physboot's
    /// HandoffPrep::UsePackageFiles() matches it against the kernel package
    /// and hands the kernel a loaded ELF. Here the phys handoff carries no
    /// userboot image, so userabi::userboot_init() looks the name up itself.
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
    /// Off by default, as in Zircon, because of what the shell is: a ring-0
    /// thread that runs whatever registered command a serial line names, with
    /// no notion of who is typing. That is the right thing for a bring-up
    /// image driven over its console and the wrong thing to get by
    /// forgetting to say otherwise, so an image that wants it asks for it
    /// (//kernel/products/image/dev/BUILD.gn). This only decides whether the
    /// shell *starts*; whether it is in the kernel at all is the build
    /// argument kernel_shell_enabled (//kernel/kernel_params.gni), and with
    /// that off this option is read and has nothing to steer.
    [[nodiscard]]
    auto shell() const -> bool {
        return m_shell;
    }

    /// `kernel.debug_read`, `kernel.debug_read=true`: permit debug_read when
    /// the caller also holds the System Debug resource. False when absent.
    [[nodiscard]]
    auto debug_read() const -> bool {
        return m_debug_read;
    }

    /// `kernel.name=<dir>`; "vmours" when absent -- phys::BootOptions'
    /// default, since both must name the same directory.
    ///
    /// The directory of the kernel package that holds this kernel's own
    /// files, under KernelPackage's fixed names (IMAGE_ELF, CODE_PATCHES).
    /// phys reads the option to find the image it loads (physboot_init()).
    /// Zircon's counterpart is `kernel.select` (lib/boot-options/options.inc),
    /// which names the kernel package to boot out of STORAGE_KERNEL.
    ///
    /// vmours has no reader of it now. Its one use was to keep the kernel's
    /// own files out of the BootFS userabi built for user space, and the
    /// kernel builds no BootFS since userboot unpacks the image's
    /// StorageBootFs item itself (.agents/tasks/userboot-bootfs). A new use
    /// must also know that this copy can disagree with phys's, where Zircon's
    /// cannot: this parses the image's Config item again and never sees the
    /// bootloader's command line (see the class comment), so `kernel.name=`
    /// typed at GRUB boots a directory other than the one named here. Such a
    /// use should take the directory from the handoff instead.
    [[nodiscard]]
    auto kernel_name() const -> std::string_view {
        return m_kernel_name;
    }

  private:
    bool m_shell{false};
    bool m_debug_read{false};

    /// A copy, NUL-terminated, rather than a view into the command line: the
    /// value is a slice of the Config item that ends at a space, and holding
    /// the slice would tie this object to the package mapping for good.
    /// 64 bytes is what phys::BootOptions keeps for `kernel.phys.next` and
    /// `kernel.name`, and what Zircon's SmallString holds this option in.
    char m_userboot[64]{"userboot"};

    /// Same size as phys::BootOptions' copy of this option, so any name phys
    /// accepts fits here too.
    char m_kernel_name[64]{"vmours"};
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
} // namespace ours::boot

#endif // OURS_BOOT_OPTIONS_HPP
