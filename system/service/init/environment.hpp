/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef INIT_ENVIRONMENT_HPP
#define INIT_ENVIRONMENT_HPP 1

#include <ours/types.hpp>

#include <span>

namespace init {
/// What starting a program takes of the root's own startup handles, none of
/// them owned: they are the root's for good. The ELF runner (realm.hpp) and
/// the Launcher (launcher_server.hpp) both start programs with
/// launch::start(), by direct call (I2).
struct Environment {
    /// The root job, which every component's process goes under, and whose
    /// duplicate it gets as JobDefault (D6). The Launcher takes the job from
    /// each Launch instead.
    ours::Uandle job;
    /// The vDSO's VMO, mapped into every process (launch::Request::vdso);
    /// each process also gets a duplicate as VmoVdso.
    ours::Uandle vdso;
    /// The clock MMIO page (HandleType::VmoClockPage), mapped at
    /// CLOCK_PAGE_BASE. Invalid when the kernel did not make one.
    ours::Uandle clock_page;
    /// The vDSO's file in the BootFS mapping, which programs are bound
    /// against.
    std::span<u8 const> vdso_file;
    /// The root's own root region, where launch::start() maps a program
    /// while it reads it.
    ours::Uandle vma;
};
} // namespace init

#endif // #ifndef INIT_ENVIRONMENT_HPP
