/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LAUNCH_LAUNCH_HPP
#define LAUNCH_LAUNCH_HPP 1

#include <span>
#include <string_view>

#include <elfload/diagnostics.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/result.hpp>

namespace launch {
/// The user-space launcher: how a program from BootFS becomes a running process.

/// What start() needs to start one program.
struct Request {
    /// The job the process goes under; it must grant ManageProcess.
    ours::Uandle job;

    /// The process's name.
    std::string_view name;

    /// The program's file as a VMO, offset 0 being the file's first byte: a
    /// slice of an executable BootFS, say. It must grant Read, Map and
    /// Duplicate, and Execute for the program's code, which is mapped from it
    /// as it is; the writable segments are snapshots of it. Not consumed.
    ours::Uandle file;

    /// The same file, readable in this process: its headers, and the dynamic
    /// tables the binder reads. Not changed.
    std::span<u8 const> file_bytes;

    /// The vDSO's VMO (HandleType::VmoVdso), which is mapped into the process
    /// whole, read and execute, sealed: the kernel takes that mapping as the
    /// registration of the process's vDSO range. Not consumed.
    ours::Uandle vdso;

    /// The vDSO as a file, the same one the kernel laid the VMO out from (the
    /// BootFS's `vdso`): the program's imports are bound to its exports.
    std::span<u8 const> vdso_file;

    /// The one handle the program starts with, its value in %rdi; usually the
    /// far end of the channel its processargs will come on. Consumed whatever
    /// start() answers, as process_start consumes it.
    ours::Uandle bootstrap;

    /// Where start() says what it did, for the caller to make into whatever it
    /// wants; none by default. Every failing step is reported once, as an
    /// Error: elfload's refusals with their own reason, and start()'s own
    /// steps with |what| naming the step, |name| the status, and |value| the
    /// segment's index or link-time address where there is one. Each import
    /// bound is reported as a Note, with the address it was bound to.
    elfload::Diagnostics diagnostics = {};
};

/// A started program, as the caller keeps it.
struct Started {
    /// A handle to the process: Initial until start() returned, running now.
    ours::Uandle process;

    /// A handle to its root region, for the processargs the caller writes on
    /// its end of the bootstrap channel next (HandleType::VmaRoot).
    ours::Uandle root_vma;
};

/// Starts the program |request| describes in a new process. The kernel's
/// userabi::launch() starts userboot in the same order of steps, but maps
/// the image at a fixed base out of one VMO, and binds it after mapping,
/// through that VMO; this reverses that. Steps 3 to 5 -- regions, relocations
/// written into the segments' VMOs, then every mapping made with its final
/// protection, RELRO read-only from the start -- are the shape of Zircon's
/// ld::RemoteDynamicLinker (Allocate, Relocate, Load); the process, the
/// stack and the start are its process_builder's. Two departures from
/// process_builder: start() starts the process itself, so the caller writes
/// the processargs message on its end of |bootstrap| afterwards (the runtime
/// waits for it); and it hands back only the process and its root region,
/// closing the thread, the image's region and the stack's VMO, which
/// process_builder passes on as ThreadSelf, LoadedVmar and StackVmo.
///
///   1. Judge the file (elfload::check_program()), read its headers
///      (Program::read()) and plan its mappings (Plan::build()), before
///      anything exists.
///   2. process_create: the process and its root region.
///   3. vma_allocate a sub-region as long as the program's segments, where the
///      kernel finds room, and the VMOs the plan asks for. A writable segment
///      is cut where its file bytes stop filling whole pages (at its end when
///      it has no .bss): the pages before the cut come from a snapshot of the
///      file, those after it from a zero-fill VMO with the partial page's
///      file bytes copied in. Either part can be empty.
///   4. Map the vDSO, and bind the program's imports to it: the relocations
///      are written into those VMOs, before anything of the program is
///      mapped. Only the vDSO comes first, since binding needs its address.
///   5. Map every mapping of the plan into the sub-region at its offset, with
///      its final protection.
///   6. A stack in a sub-region of its own, with a guard page below it: the
///      size the program asks for with PT_GNU_STACK, or 64 KiB.
///   7. thread_create and process_start, at the entry point with the stack,
///      |bootstrap| in %rdi and the vDSO's base in %rsi.
///
/// On an error the process is taken down again (closing the last handle to
/// a process that never started destroys it), and nothing is left in this
/// process but what it had. The status is the answer of the step that
/// failed, which is not always a system call:
///   - judging, before anything exists, the program or the vDSO: InvArgs for
///     a file check_program() refuses (more than its 12 PT_LOADs among the
///     reasons) or a layout Plan::build() refuses; OutOfRange for more
///     PT_LOADs than a Plan's 8, or a PT_GNU_STACK size that would wrap;
///     NotSupported for a second PT_GNU_RELRO;
///   - binding: NotFound for an import the vDSO does not export,
///     NotSupported for a relocation the binder does not apply, OutOfRange
///     or InvArgs for one outside the image or on a page that comes straight
///     from the file;
///   - otherwise, the answer of the system call that failed.
/// Whichever step it was is also reported to |diagnostics| (see Request).
[[nodiscard]]
auto start(Request const &request) -> ustl::Result<Started, ours::Status>;
} // namespace launch

#endif // #ifndef LAUNCH_LAUNCH_HPP
