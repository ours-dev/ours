#include <ours/arch/aspace_layout.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_mapping.hpp>
#include <ours/mm/vm_object.hpp>
#include <ours/object/job_dispatcher.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/object/vma_dispatcher.hpp>
#include <ours/time/clock.hpp>

#include <algorithm>
#include <utility>

#include <gktl/counter.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>

GKTL_COUNTER(DISPATCHER_PROCESS_CREATE_COUNT, "dispatcher.process.create");
GKTL_COUNTER(DISPATCHER_PROCESS_DESTROY_COUNT, "dispatcher.process.destroy");

namespace {
constexpr auto PRIVATE_ASPACE_BASE = USER_ASPACE_BASE;

constexpr auto PRIVATE_ASPACE_SIZE = USER_ASPACE_RESTRICTED_SIZE;

[[maybe_unused]]
constexpr auto SHARED_ASPACE_BASE = PRIVATE_ASPACE_BASE + PRIVATE_ASPACE_SIZE + PAGE_SIZE;

[[maybe_unused]]
constexpr auto SHARED_ASPACE_SIZE = USER_ASPACE_SIZE - USER_ASPACE_RESTRICTED_SIZE;

} // namespace

namespace ours::object {
ProcessDispatcher::ProcessDispatcher(ustl::Arc<JobDispatcher> job, char const *const name,
                                     ustl::Arc<mm::VmAspace> aspace,
                                     ustl::Arc<HandleTable> handle_table,
                                     ustl::Arc<HandleAccount> handle_account)
    : Base()
    , m_parent(nullptr)
    , m_state(State::Initial)
    , m_retcode(0)
    , m_start_time(0)
    , m_name(name)
    , m_job(ustl::move(job))
    , m_aspace(ustl::move(aspace))
    , m_handle_table(ustl::move(handle_table))
    , m_handle_account(ustl::move(handle_account))
    , m_exceptionate() {}

/// The last reference is gone. Counted and logged because nothing else
/// witnesses it: a killed process has no parent that waits on it yet (P1-B),
/// and the line is how a test -- or a reader of the log -- tells that the
/// whole chain let go, thread to process to handle table, rather than that
/// something still holds a reference and the process lives on unseen.
/// Off the job's list first, for a process that never died -- one dropped
/// while its creator was still building it. Before anything of it goes, so a
/// job walking its list under its lock never meets a process half destroyed.
ProcessDispatcher::~ProcessDispatcher() {
    m_job->remove_process(*this);
    DISPATCHER_PROCESS_DESTROY_COUNT.add(1);
    log::info("object: process '{}' (koid {}) destroyed", name(), koid());
}

auto ProcessDispatcher::create(ustl::Arc<JobDispatcher> &job, char const *const name,
                               KernelHandle<Self> &process_out, Rights &process_rights,
                               KernelHandle<VmaDispatcher> &root_vma_out, Rights &root_vma_rights)
    -> Status {
    // The restricted range and not the full user half. USER_ASPACE_SIZE runs
    // to the 2^47 canonical split; PRIVATE_ASPACE_SIZE stops at 2^46 and
    // leaves the upper half of the user range for the shared aspace
    // create_shared() will hand out, which only works if the private one is
    // carved to that boundary from the start rather than shrunk later.
    ustl::Arc<mm::VmAspace> aspace;
    auto status = mm::VmAspace::create(PRIVATE_ASPACE_BASE, PRIVATE_ASPACE_SIZE, mm::VmasKind::User,
                                       mm::VmasFlags::None, name, &aspace);
    if (Status::Ok != status) {
        return status;
    }

    // The root VMA is the address space's own, created by VmAspace::init();
    // this only puts an object-layer face on it. That is the whole reason
    // create() hands back two objects: a process is not usable without a way
    // to map into it, and the only thing that may map is a handle.
    auto root_vma = aspace->root_vma();
    if (!root_vma) {
        return Status::InternalError;
    }

    status = VmaDispatcher::create(ustl::move(root_vma), root_vma_out, root_vma_rights);
    if (Status::Ok != status) {
        return status;
    }

    // Last, because it takes ownership of the aspace: everything above can
    // still fail and unwind by simply dropping Arcs, and after this line the
    // aspace has an owner that outlives this frame. A failure here leaves
    // root_vma_out holding a dispatcher, which the caller's KernelHandle
    // releases on its own -- and the aspace dies with it, since the root VMA
    // was the last thing referencing it.
    auto handle_table = ustl::Arc<HandleTable>::adopt(new (mm::GAF_KERNEL) HandleTable());
    if (!handle_table) {
        return Status::OutOfMem;
    }

    auto handle_account = ustl::Arc<HandleAccount>::adopt(new (mm::GAF_KERNEL) HandleAccount());
    if (!handle_account) {
        return Status::OutOfMem;
    }

    auto self = ustl::Arc<Self>::adopt(new (mm::GAF_USER) Self(
        job, name, ustl::move(aspace), ustl::move(handle_table), ustl::move(handle_account)));
    if (!self) {
        return Status::OutOfMem;
    }

    // Listed once whole: nothing above can fail any more. Its job hides it
    // until it has a handle or has started (is_unpublished()).
    job->add_process(*self);

    process_rights = default_right();
    process_out = KernelHandle<Self>(ustl::move(self));

    DISPATCHER_PROCESS_CREATE_COUNT.add(1);
    return Status::Ok;
}

auto ProcessDispatcher::current() -> Self * {
    auto *const thread = ThreadDispatcher::current();
    if (thread == nullptr) {
        return nullptr;
    }
    return &thread->process();
}

/// The order the steps are in is the whole of it. The handle goes into the
/// table before the thread starts, because a thread that is runnable can run
/// this instant, on another CPU, and its first act may be to use the value it
/// was given; put in after, the value would name nothing for as long as this
/// thread took to get there, and correctness would rest on how fast it was.
/// Put in before, nobody can reach the table yet -- the process has no thread
/// -- so nothing is missing from it and nothing can see the handle early,
/// bar the one thing that also touches the table: the process's own teardown
/// (finish_dead_transition() clears it), which is why the failure path below
/// takes the handle out by value and does not assume it is still there.
///
/// On failure the handle is closed here, after the process's lock is gone
/// (attach_thread() takes and drops it): closing a handle can run its object's
/// on_zero_handles(), which must not nest inside this process's lock. If the
/// process has already gone Dead, its table is empty and the removal finds
/// nothing; the table's own lock is what decides which of the two -- the
/// teardown or this -- closes the handle, and only one of them does.
///
/// The check for a vDSO is here and not in the thread: a process is started,
/// a thread only attached, and "could not make a system call" is about the
/// process. It reads the range without the lock, as vdso_contains() does; the
/// range is registered once, by the mapping of the vDSO's VMO, and changes
/// after only if that vma_map cannot report the address and withdraws it
/// again (VmaDispatcher::undo_map()). A start racing that withdrawal starts a
/// process whose system calls are refused from then on, as for any process
/// without a vDSO: the program that maps and starts it raced itself.
auto ProcessDispatcher::start(ThreadDispatcher &thread, VirtAddr const entry, VirtAddr const stack,
                              ustl::Box<Handle> arg1, usize const arg2) -> Status {
    verify_canary();
    DEBUG_ASSERT(&thread.process() == this, "a process starts one of its own threads");

    if (!m_aspace->has_vdso()) {
        return Status::BadState;
    }

    auto arg1_value = ours::Uandle::Invalid;
    if (arg1) {
        arg1_value = m_handle_table->add(ustl::move(arg1));
    }

    // The handle goes out as a number in the first argument register: the
    // register is where it stops being a handle, as from_register() is where
    // a handle argument starts being one.
    auto const status = thread.start(
        {.entry = entry, .stack = stack, .arg1 = std::to_underlying(arg1_value), .arg2 = arg2},
        true);
    if (Status::Ok != status && arg1_value != ours::Uandle::Invalid) {
        auto _ = m_handle_table->remove(arg1_value);
    }
    return status;
}

/// Makes |thread| one of this process's running threads.
///
/// One lock, the process's own, over the three things that have to change
/// together: the state check, starting the thread, and the list. With the
/// list under a lock of its own -- as it used to be -- a kill walking the list
/// could run between the thread starting and the thread being listed, and
/// miss a thread that was already running; or a thread could be added to a
/// process that had begun dying. Under one lock the process is, to anyone
/// who takes that lock, either without the thread or with it running.
///
/// The first thread moves the process from Initial to Running; any later one
/// requires Running, so that nothing is added to a process that is dying. The
/// caller says which it is, and the state has to agree: inferring it from an
/// empty list would let a second "first" start -- two callers of process_start,
/// or one that had lost the race -- pass as an ordinary later thread of a
/// process that is already Running.
///
/// Lock order: this process's lock, then the thread's (activate() takes it).
auto ProcessDispatcher::attach_thread(ThreadDispatcher &thread, bool const first_thread,
                                      ThreadDispatcher::TaskState const &entry) -> Status {
    verify_canary();

    Guard guard{get_lock()};

    auto const expected = first_thread ? State::Initial : State::Running;
    if (m_state != expected) {
        return Status::BadState;
    }
    DEBUG_ASSERT(m_thread_list.empty() == first_thread,
                 "a process is Initial exactly while it has no thread");

    // The kernel thread is made here, under the lock and after the state
    // check, and not when the thread was created: a start that finds the
    // process Dead has made nothing, so there is nothing to dispose of, and a
    // failure to make it (no memory for a stack) leaves the state and the
    // list untouched. The cost is that everything a start does runs inside
    // this lock, including allocating a kernel stack; how long that takes
    // and whether anything waits for it is to be measured
    // (docs/repairs/260928/kernel-object.md).
    if (auto const status = thread.create_kernel_thread(); Status::Ok != status) {
        return status;
    }

    thread.mark_active();
    if (auto const status = thread.activate(entry, first_thread, false); Status::Ok != status) {
        return status;
    }
    m_thread_list.push_back(thread);

    if (first_thread) {
        // Under the same lock as the state: a reader that sees Running sees
        // the time.
        DEBUG_ASSERT(m_start_time == 0, "a process starts once");
        m_start_time = time::SteadyClock::now().time_since_epoch().count();
        m_state = State::Running;
    }
    return Status::Ok;
}

/// One critical section holds both the state check and the change, and the
/// same lock is what attach_thread() checks Initial under, so a start racing
/// this has exactly two outcomes and no third: it took the lock first and the
/// process is Running, and this finds so and does nothing; or this took it
/// first, and the start finds Dead and answers BadState. Without the one lock
/// a start could pass its check, this could then make the process Dead, and
/// the start would attach a thread to an address space being torn down.
///
/// A reference is not a handle. The starter holds an Arc to the process while
/// it works, so the object's memory outlives its last handle; what this
/// decides is the process's *state*, which the reference count says nothing
/// about. Hence the check under the lock rather than an assumption that a
/// caller who holds the process is holding a handle to it.
///
/// The teardown runs after the lock is released, exactly once: the lock is
/// what makes this edge, and the ones in kill() and detach_thread(), each
/// taken by at most one caller (see finish_dead_transition()).
auto ProcessDispatcher::on_zero_handles() -> void {
    verify_canary();

    {
        Guard guard{get_lock()};
        if (m_state != State::Initial) {
            return;
        }
        m_state = State::Dead;
    }
    finish_dead_transition();
}

auto ProcessDispatcher::kill(i64 const retcode) -> void {
    verify_canary();

    auto became_dead = false;
    {
        Guard guard{get_lock()};
        if (m_state == State::Dead || m_state == State::Dying) {
            return;
        }
        m_retcode = retcode;

        // No thread to wait for: straight to Dead, and the rest of dying
        // happens below, outside the lock.
        if (m_thread_list.empty()) {
            m_state = State::Dead;
            became_dead = true;
        } else {
            // Dying, and every thread told. Under the lock that
            // attach_thread() and detach_thread() take, so no thread can be
            // added after this, and none is half-way out of the list while
            // this walks it. Each kill only raises a signal: the threads act
            // on it themselves, on their way back to ring 3, which is also why
            // this can hold a sleeping lock while it runs. The last of them to
            // leave finishes the process (detach_thread()).
            m_state = State::Dying;
            for (auto &thread: m_thread_list) {
                thread.kill();
            }
        }
    }

    if (became_dead) {
        finish_dead_transition();
    }
}

/// The process ends with |retcode|, by its own hand: it starts dying, every
/// other thread in it is told to die, and the calling thread exits here. The
/// last thread out -- this one, or whichever of the others leaves last --
/// makes it Dead (detach_thread()).
///
/// The code is recorded only on the way into Dying. A process already dying
/// keeps the code it is dying with: a kill that got there first, or an exit
/// racing on another thread, decided it, and two threads calling exit at once
/// must not leave the answer to whichever took the lock second.
///
/// The calling thread is not killed with the others. It is on its way out
/// already, and killing the current thread would only raise a signal it never
/// returns to ring 3 to act on.
auto ProcessDispatcher::exit(i64 const retcode) -> void {
    verify_canary();

    auto *const current = ThreadDispatcher::current();
    DEBUG_ASSERT(current != nullptr && &current->process() == this,
                 "exit() is called on a thread of the exiting process");
    {
        Guard guard{get_lock()};
        DEBUG_ASSERT(m_state == State::Running || m_state == State::Dying,
                     "a process with a thread running is Running or Dying");
        if (m_state != State::Dying) {
            m_retcode = retcode;
            m_state = State::Dying;
            for (auto &thread: m_thread_list) {
                if (&thread != current) {
                    thread.kill();
                }
            }
        }
    }

    ThreadDispatcher::exit_current();
}

/// Takes |thread| out of this process, as its kernel thread exits.
///
/// Under the process's lock, like attach_thread(), so that a kill walking the
/// list sees each thread either still listed and alive or gone entirely --
/// never listed but already without a kernel thread.
///
/// The last thread out makes the process Dead, whether it was killed (Dying)
/// or simply the last to finish (Running). This used to leave the state
/// alone: only "killed with no threads" ever reached Dead, and a process whose
/// threads all exited stayed Running forever, its handles open until the last
/// reference to the process object happened to go -- which a handle to the
/// process held anywhere, including in its own table, could put off for good.
/// The rest of dying, finish_dead_transition(), runs after the lock is
/// dropped.
auto ProcessDispatcher::detach_thread(ThreadDispatcher &thread) -> Status {
    verify_canary();

    auto became_dead = false;
    {
        Guard guard{get_lock()};
        thread.mark_dead();
        auto _ = m_thread_list.erase(m_thread_list.iterator_to(thread));
        if (m_thread_list.empty()) {
            DEBUG_ASSERT(m_state == State::Running || m_state == State::Dying,
                         "a process with threads is Running or Dying");
            m_state = State::Dead;
            became_dead = true;
        }
    }

    // The thread's TERMINATED once it is off the list, outside this lock --
    // telling its observers takes their own locks -- and before the process's
    // own: a waiter woken by the process's finds its threads finished too.
    thread.update_state(Signals::ThreadRunning | Signals::ThreadSuspended, Signals::Terminated);

    if (became_dead) {
        finish_dead_transition();
    }
    return Status::Ok;
}

/// What a process does once it is Dead, outside its lock and exactly once:
/// the three ways in -- on_zero_handles() and kill() on a process with no
/// thread (Initial -> Dead), detach_thread() of the last thread (Running or
/// Dying -> Dead) -- each set Dead under the lock from a state that is not
/// Dead, and nothing leaves Dead, so only one of them ever gets here.
///
/// Four parts. The exception channel is shut first (see below). The address
/// space is torn down, so the process's memory comes back now rather than
/// never. And the handle table is emptied, so every object the process held a
/// handle to sees it go now -- the peer of each channel it held reads
/// PEER_CLOSED -- rather than whenever the process object itself is
/// destroyed. Outside the lock because both can run other objects'
/// destructors (a VMO's, on unmapping; on_zero_handles(), on closing), which
/// take those objects' locks and must not nest inside this one. TERMINATED,
/// the fourth part, is raised last, when the rest is done.
///
/// The table object itself stays: it is empty and lives on with the process,
/// which is harmless and keeps handle_table() valid for anyone still holding
/// the process.
auto ProcessDispatcher::finish_dead_transition() -> void {
    // Off the job's list: a dead process is neither listed nor found by koid.
    // Outside the process lock, as the Job -> Process order needs.
    m_job->remove_process(*this);

    // The exception channel before anything else. Its reader sees PeerClosed,
    // which is how a supervisor learns the process it watches is gone, and a
    // channel bound from now on is refused. It also breaks the one reference
    // cycle a dead process can be caught in: process -> exceptionate -> kernel
    // end <-> user end -> an unread exception message -> its handle -> the
    // exception -> the thread -> the process. Outside the process lock, as
    // everything here is: dropping the kernel end runs its on_zero_handles()
    // (hard rule 1, userspace-interface.md 4.8).
    m_exceptionate.shutdown();

    // Then the address space: every mapping goes, and every VMO only this
    // process referenced -- its image, its stack -- with it. m_aspace itself
    // is kept until this object is destroyed: the process's threads reach it
    // through here (aspace()), and the last of them is still running, on these
    // page tables, as this runs.
    auto _ = m_aspace->destroy();

    auto const closed = m_handle_table->clean();
    log::debug("object: process '{}' (koid {}) dead, {} handles closed", name(), koid(), closed);

    // TERMINATED last, once everything above is done: a waiter woken by it may
    // look at the process at once -- read its return code, count on its memory
    // being back -- and has to find the process finished rather than
    // finishing. The code is read under the lock it is guarded by, though
    // nothing writes it any more: every writer requires a state before Dead.
    auto const retcode = [this] {
        Guard guard{get_lock()};
        return m_retcode;
    }();
    update_state(Signals::None, Signals::Terminated);
    log::debug("object: process '{}' (koid {}) terminated, retcode {}", name(), koid(), retcode);
}

auto ProcessDispatcher::related_koid() const -> KoId {
    return m_job->koid();
}

auto ProcessDispatcher::job() -> JobDispatcher & {
    return m_job.as_ref_mut();
}

/// Started from Running on, and Exited from Dying on. A process killed before
/// it started (Initial straight to Dead) so reports both, with a start time
/// of zero.
auto ProcessDispatcher::get_info() -> InfoProcess {
    verify_canary();

    State state;
    InfoProcess info{};
    {
        Guard guard{get_lock()};
        state = m_state;
        info.return_code = m_retcode;
        info.start_time = m_start_time;
    }

    switch (state) {
        case State::Dead:
        case State::Dying:
            info.flags |= InfoProcessFlags::Exited;
            [[fallthrough]];
        case State::Running:
            info.flags |= InfoProcessFlags::Started;
            break;
        case State::Initial:
            break;
    }
    return info;
}

namespace {
    /// |name| into |out|, cut to fit and NUL-terminated; an empty name for null.
    template <usize N>
    auto copy_name(char (&out)[N], char const *const name) -> void {
        usize length = 0;
        if (name != nullptr) {
            while (length + 1 < N && name[length] != '\0') {
                out[length] = name[length];
                ++length;
            }
        }
        out[length] = '\0';
    }

    /// A range's protection in the ABI's words.
    auto to_vm_options(mm::MmuFlags const mmuf) -> VmOptions {
        auto options = VmOptions::None;
        if (!!(mmuf & mm::MmuFlags::Readable)) {
            options |= VmOptions::PermRead;
        }
        if (!!(mmuf & mm::MmuFlags::Writable)) {
            options |= VmOptions::PermWrite;
        }
        if (!!(mmuf & mm::MmuFlags::Executable)) {
            options |= VmOptions::PermExecute;
        }
        return options;
    }

    /// One pass over a process's maps: the records kept, up to their capacity,
    /// and every record counted. The counting pass keeps none.
    struct MapsPass {
        ktl::Vec<InfoMaps> *records;
        usize count;
    };

    /// Keeps |record| while there is room, and counts it either way. The room is
    /// the capacity reserved before the pass, so the push never allocates: it
    /// runs under the address space's lock.
    auto keep(MapsPass &pass, InfoMaps const &record) -> void {
        if (pass.records != nullptr && pass.records->size() < pass.records->capacity()) {
            auto _ = pass.records->push_back(record); // within capacity, so it cannot fail
        }
        ++pass.count;
    }

    /// for_each_node()'s visitor: a record for an area, and one per protection
    /// range for a mapping. Under the address space's lock, so it reads nodes and
    /// takes only an object's lock, to count committed pages, and only for a
    /// record it will keep.
    auto visit_maps_node(void *const context, mm::VmAreaOrMapping &node, usize const depth)
        -> void {
        auto &pass = *static_cast<MapsPass *>(context);
        if (!node.is_mapping()) {
            InfoMaps record{};
            copy_name(record.name, node.name());
            record.base = node.base();
            record.size = node.size();
            record.depth = depth;
            record.type = InfoMapsType::Vma;
            keep(pass, record);
            return;
        }

        auto &mapping = static_cast<mm::VmMapping &>(node);
        mapping.get_lock().assert_held();
        auto vmo = mapping.vmo();
        char vmo_name[MAX_NAME_LEN] = {};
        vmo->get_name(vmo_name, sizeof(vmo_name));
        auto const vmo_koid = vmo->user_id();
        mapping.for_each_protection_range_locked([&](VirtAddr const base, usize const size,
                                                     mm::MmuFlags const mmuf) {
            auto const vmo_offset = mapping.vmo_offset() + (base - mapping.base());
            auto const kept =
                pass.records != nullptr && pass.records->size() < pass.records->capacity();
            InfoMaps record{};
            copy_name(record.name, vmo_name);
            record.base = base;
            record.size = size;
            record.depth = depth;
            record.type = InfoMapsType::Mapping;
            record.u.mapping.mmu_flags = to_vm_options(mmuf);
            record.u.mapping.vmo_koid = vmo_koid;
            record.u.mapping.vmo_offset = vmo_offset;
            if (kept) {
                record.u.mapping.committed_pages = vmo->committed_pages(vmo_offset, size);
            }
            keep(pass, record);
        });
    }
} // namespace

/// Two walks: the first counts, the second fills what the first made room for.
/// The space can change between them -- a thread of the process maps or
/// unmaps -- so the second counts again, and |avail| is its count; the
/// records never outgrow their room, since the push is bounded by it.
auto ProcessDispatcher::get_maps(usize const max, ktl::Vec<InfoMaps> *const records,
                                 usize *const avail) -> Status {
    verify_canary();

    // The address space's own record, which no walk visits, is counted here.
    auto counting = MapsPass{nullptr, 1};
    if (auto const status = m_aspace->for_each_node(&counting, &visit_maps_node);
        status != Status::Ok) {
        return status;
    }
    if (records->reserve(std::min(max, counting.count)).is_err()) {
        return Status::NoMemory;
    }

    auto filling = MapsPass{records, 0};
    InfoMaps aspace{};
    copy_name(aspace.name, m_aspace->name());
    aspace.base = m_aspace->base();
    aspace.size = m_aspace->size();
    aspace.depth = 0;
    aspace.type = InfoMapsType::Aspace;
    keep(filling, aspace);
    if (auto const status = m_aspace->for_each_node(&filling, &visit_maps_node);
        status != Status::Ok) {
        return status;
    }
    *avail = filling.count;
    return Status::Ok;
}

auto ProcessDispatcher::is_unpublished() -> bool {
    Guard guard{get_lock()};
    return m_state == State::Initial && handle_count() == 0;
}

auto ProcessDispatcher::lookup_thread(KoId const koid) -> ustl::Arc<ThreadDispatcher> {
    Guard guard{get_lock()};
    for (auto &thread: m_thread_list) {
        if (thread.koid() == koid) {
            return ustl::Arc<ThreadDispatcher>::try_adopt(&thread);
        }
    }
    return {};
}

/// The mapping is found under the address space's lock and read without it:
/// the reference find_mapping() hands out keeps the mapping and its object
/// alive, and the read may fault on the caller's buffer, which takes the
/// caller's own address space lock. A mapping unmapped between the two is
/// still read, from the object it showed: the race Zircon's read has too.
auto ProcessDispatcher::read_memory(VirtAddr const va, user_copy::UserOutPtr<u8> const out,
                                    usize const size, usize *const actual) -> Status {
    verify_canary();

    ustl::Arc<mm::VmMapping> mapping;
    if (auto const status = m_aspace->find_mapping(va, &mapping); status != Status::Ok) {
        return status;
    }
    // Cut at the mapping's end: the next byte is another mapping's, or none.
    auto const into = va - mapping->base();
    auto const length = std::min<usize>(size, mapping->size() - into);
    auto const offset = mapping->vmo_offset() + into;

    // The whole buffer, before the first page is read, as vmo_read checks it:
    // reading a page nobody has written commits a zeroed one in this process,
    // and a call that is bound to fail should not cost it memory.
    if (!user_copy::is_user_range(out.address(), length)) {
        return Status::InvArgs;
    }
    auto vmo = mapping->vmo();
    if (length > vmo->size() || offset > vmo->size() - length) {
        return Status::OutOfRange;
    }
    if (auto const status = vmo->read_user(out, VirtAddr(offset), length); status != Status::Ok) {
        return status;
    }
    *actual = length;
    return Status::Ok;
}

/// No lock of the process's: the ports are the address space's, under its
/// I/O bitmap's own lock. A process that is dying may still open some; no
/// thread of it reaches ring 3 to use them.
auto ProcessDispatcher::open_ioports(u32 const base, u32 const count) -> Status {
    verify_canary();
    return m_aspace->arch_aspace().open_ioports(base, count);
}

auto ProcessDispatcher::close_ioports(u32 const base, u32 const count) -> Status {
    verify_canary();
    return m_aspace->arch_aspace().close_ioports(base, count);
}

} // namespace ours::object