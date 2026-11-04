/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/init.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/object/job_dispatcher.hpp>
#include <ours/panic.hpp>

#include <algorithm>

#include <gktl/init_hook.hpp>
#include <heap/new.hpp>

namespace ours::object {
namespace {
    /// The one job every process created so far belongs to.
    ///
    /// A file-scope Arc rather than a function-local static: a local static
    /// with a non-constant initialiser needs __cxa_guard_acquire, which this
    /// freestanding kernel does not link, and the guard would be paying for a
    /// race that cannot happen anyway (see the init hook below).
    ustl::Arc<JobDispatcher> ROOT_JOB;

    /// Built from an init hook rather than lazily on first use, for the same
    /// reason VmAspace::init_kernel_aspace() is: the boot sequence is the only
    /// thing that knows when the heap is up, and a lazy first-use path would
    /// have to be either thread-safe or documented as boot-only -- and the
    /// second is what the hook enforces for free.
    ///
    /// Vmm + 1, not Heap: the object is a plain heap allocation, but the first
    /// thing that will ever ask for it is process creation, which needs the
    /// VMM. Registering it any earlier only widens the window in which a
    /// half-built system could reach it.
    INIT_CODE
    auto init_root_job() -> void {
        ROOT_JOB = ustl::Arc<JobDispatcher>::adopt(new (mm::ALLOC_KERNEL) JobDispatcher());
        if (!ROOT_JOB) {
            panic("Failed to create the root job");
        }
    }
    GKTL_INIT_HOOK(JobRootInit, init_root_job, gktl::InitLevel::Vmm + 1);
} // namespace

auto JobDispatcher::root_job() -> ustl::Arc<Self> & {
    // Not a soft failure: every process belongs to a job, so a caller that
    // gets here before the hook above has run would go on to build a process
    // with no job at all rather than fail visibly.
    ASSERT(ROOT_JOB, "The root job has not been created yet");
    return ROOT_JOB;
}

auto JobDispatcher::add_process(ProcessDispatcher &process) -> void {
    Guard guard{get_lock()};
    m_processes.push_back(process);
}

auto JobDispatcher::remove_process(ProcessDispatcher &process) -> void {
    Guard guard{get_lock()};
    if (process.m_job_hook.is_linked()) {
        m_processes.erase(process);
    }
}

/// Two passes under the lock around one allocation without it, as the maps
/// walk does: the list can change between them, so the second counts again,
/// and it keeps a koid only while there is room, so it never allocates under
/// the lock. is_unpublished() takes each process's lock inside this one,
/// the Job -> Process order.
auto JobDispatcher::get_process_koids(usize const max, ktl::Vec<KoId> *const koids,
                                      usize *const avail) -> Status {
    usize count = 0;
    {
        Guard guard{get_lock()};
        for (auto &process: m_processes) {
            if (!process.is_unpublished()) {
                ++count;
            }
        }
    }
    if (koids->reserve(std::min(max, count)).is_err()) {
        return Status::NoMemory;
    }

    count = 0;
    Guard guard{get_lock()};
    for (auto &process: m_processes) {
        if (process.is_unpublished()) {
            continue;
        }
        if (koids->size() < koids->capacity()) {
            auto _ = koids->push_back(process.koid()); // within capacity, so it cannot fail
        }
        ++count;
    }
    *avail = count;
    return Status::Ok;
}

auto JobDispatcher::lookup_process(KoId const koid) -> ustl::Arc<ProcessDispatcher> {
    Guard guard{get_lock()};
    for (auto &process: m_processes) {
        if (process.koid() == koid) {
            return ustl::Arc<ProcessDispatcher>::try_adopt(&process);
        }
    }
    return {};
}

/// The exceptionate is made and bound before the lock, so nothing is
/// allocated under it. Asking each one bound whether its reader is still
/// there takes the exceptionate's lock inside the job's, the Job ->
/// Exceptionate order. A refused one is released after the lock, with the
/// ones pruned: |gone| is declared before the guard, so it goes after it.
auto JobDispatcher::bind_debugger(KernelHandle<ChannelDispatcher> channel,
                                  Rights const thread_rights, Rights const process_rights)
    -> Status {
    m_canary.verify<Self>();

    auto debugger = ustl::Arc<DebugExceptionate>::adopt(new (mm::ALLOC_KERNEL) DebugExceptionate());
    if (!debugger) {
        return Status::NoMemory;
    }
    if (auto const bound =
            debugger->set_channel(ustl::move(channel), thread_rights, process_rights);
        bound != Status::Ok) {
        return bound;
    }

    ustl::Arc<DebugExceptionate> gone[MAX_JOB_DEBUGGER_CHANNELS];
    Guard guard{get_lock()};
    auto const kept = prune_debuggers_locked(gone);
    if (kept == MAX_JOB_DEBUGGER_CHANNELS) {
        return Status::AlreadyBound;
    }
    m_debuggers[kept] = ustl::move(debugger);
    m_debugger_count = kept + 1;
    return Status::Ok;
}

/// Pruned here too, so that a channel whose reader has gone is closed at the
/// next start rather than kept until the next bind. The snapshot holds a
/// reference to each one, so a bind that drops one meanwhile drops only the
/// job's.
auto JobDispatcher::for_each_debugger(void *const context, DebuggerVisitor const visit) -> void {
    m_canary.verify<Self>();

    ustl::Arc<DebugExceptionate> gone[MAX_JOB_DEBUGGER_CHANNELS];
    ustl::Arc<DebugExceptionate> snapshot[MAX_JOB_DEBUGGER_CHANNELS];
    usize count = 0;
    {
        Guard guard{get_lock()};
        count = prune_debuggers_locked(gone);
        for (usize i = 0; i < count; ++i) {
            snapshot[i] = m_debuggers[i];
        }
    }
    for (usize i = 0; i < count; ++i) {
        if (!visit(context, *snapshot[i])) {
            break;
        }
    }
}

auto JobDispatcher::prune_debuggers_locked(
    ustl::Arc<DebugExceptionate> (&gone)[MAX_JOB_DEBUGGER_CHANNELS]) -> usize {
    usize kept = 0;
    for (usize i = 0; i < m_debugger_count; ++i) {
        if (!m_debuggers[i]->has_valid_channel()) {
            gone[i] = ustl::move(m_debuggers[i]);
        } else if (kept != i) {
            m_debuggers[kept++] = ustl::move(m_debuggers[i]);
        } else {
            ++kept;
        }
    }
    m_debugger_count = kept;
    return kept;
}
} // namespace ours::object
