/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_MODULE_HPP
#define OURS_PHYS_MODULE_HPP 1

#include <ours/compiler.hpp>
#include <ours/phys/physload.hpp>
#include <ours/types.hpp>

namespace ours::phys {

/// What a phys module tells physload about itself.
///
/// A module does not have an entry point in the ELF sense.  It declares
/// one, the way a Linux kernel module does with module_init(): the
/// PHYS_MODULE() macro plants this record in the `.phys.module` section,
/// and ModuleLoader reads it after loading and binding the image.  Nothing
/// on either side depends on a particular function name, and physload can
/// refuse a module whose ABI it does not speak instead of jumping into it.
struct PhysModule {
    /// Bumped whenever the shape of this record or the meaning of `init`
    /// changes.  A module built against an older physload fails the load
    /// with a diagnostic rather than running against the wrong contract.
    static constexpr u32 kAbiVersion = 1;

    /// Section physload looks in, and the section PHYS_MODULE() writes to.
    static constexpr char const *kSection = ".phys.module";

    u32 abi_version;
    u32 reserved;

    /// For diagnostics.  It need not match the BootFS file name the module
    /// was loaded from -- physload reports both when they differ.
    char const *name;

    /// Called once, after physload has relocated the image, bound its
    /// imports and run its static constructors.  Never returns: the module
    /// either boots something or panics.  (The contract is on the callee;
    /// [[noreturn]] is not part of a function pointer's type.)
    auto (*init)(PhysLoadHandoff const &handoff) -> void;
};

} // namespace ours::phys

/// Declare this translation unit's module identity and entry.
///
///     PHYS_MODULE("physboot", physboot_init);
///
/// One per module.  `used`/`retain` keep the record through --gc-sections,
/// which would otherwise drop it: nothing in the module refers to it, and
/// the only consumer is a loader reading the section from outside.
#define PHYS_MODULE(module_name, init_function)                                                    \
    namespace {                                                                                    \
    [[gnu::used, gnu::retain, gnu::section(".phys.module")]]                                        \
    ::ours::phys::PhysModule const g_phys_module{                                                   \
        .abi_version = ::ours::phys::PhysModule::kAbiVersion,                                       \
        .reserved = 0,                                                                              \
        .name = (module_name),                                                                      \
        .init = (init_function),                                                                    \
    };                                                                                             \
    } /* namespace */

#endif // #ifndef OURS_PHYS_MODULE_HPP
