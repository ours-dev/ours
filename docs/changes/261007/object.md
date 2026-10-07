# object changes

## object_get_info answers HandleBasic: a handle's koid, rights, type and related koid (2026-10-07)

**Files**
- `system/public/ours/object_info.hpp` — new `InfoHandleBasic` record, 32 bytes, layout pinned by static_asserts; the topic comment says HandleBasic is answered.
- `system/public/ours/syscalls.gni` — `object_get_info`'s documentation: HandleBasic takes a handle of any type and asks for no right.
- `kernel/object/include/ours/object/dispatcher.hpp` — `Dispatcher::related_koid()`, virtual, 0 by default; `PeeredDispatcher` answers `m_peer_koid`.
- `kernel/object/include/ours/object/process_dispatcher.hpp`, `kernel/object/process_dispatcher.cpp` — a process answers its job's koid.
- `kernel/object/include/ours/object/thread_dispatcher.hpp`, `kernel/object/thread_dispatcher.cpp` — a thread answers its process's koid.
- `kernel/object/include/ours/object/handle_table.hpp`, `kernel/object/handle_table.cpp` — `HandleTable::get_untyped()`: the object and the handle's rights, with no type or right required.
- `kernel/lib/syscall/src/object.cpp` — the HandleBasic case.
- `examples/shell/shell_tests.cpp` — new `check_handle_basic()` on both images; the probe check that expected NotSupported for HandleBasic now asks for topic 4, which nothing names; `check_exception_tasks()`'s comment no longer says the koids cannot be had.

**Symptom**
A program could hold a handle and still not know which object it named. `InfoTopic::HandleBasic` was numbered in the ABI but answered NotSupported. The profile program, holding `ort::process_self()`, still could not read its own koid to match against the `pid` in its samples.

**Cause**
Not a mistake so much as a step not taken yet: K24 answered only the Process topic. HandleBasic was left named and unanswered on purpose, as the example of "a topic the ABI numbers that this kernel does not answer".

What was missing is worth spelling out, because it shows what a handle is. The koid is a property of the object. The rights are a property of the handle: two handles to one process can carry different rights. Only the kernel sees both, since the handle table is kernel memory and user space holds nothing but the table index. So no user-space library could have worked either one out.

**Change**
The syscall looks the handle up without naming a type and without requiring a right, then builds the record outside the table lock:

```cpp
case InfoTopic::HandleBasic: {
    ustl::Arc<object::Dispatcher> dispatcher;
    auto rights = Rights::None;
    if (auto const status = ...handle_table().get_untyped(handle, &dispatcher, &rights); ...)
    auto const record = InfoHandleBasic{
        .koid = dispatcher->koid(),
        .rights = rights,
        .type = dispatcher->type(),
        .related_koid = dispatcher->related_koid(),
        .padding1 = {},
    };
    return single_record_result(buffer, buffer_size, actual, avail, record);
}
```

There is no right check, unlike Process, which needs Inspect. The record describes the caller's own handle. A holder can already learn the rights by trying calls, and the type by watching which ones answer BadType. Hiding the record would cost callers a probe and protect nothing.

`related_koid` is answered for the three objects bound to another from birth, as the user chose (option A):
- a process answers `m_job->koid()`;
- a thread answers `m_process->koid()`;
- a channel end answers `m_peer_koid`.

Every other object answers 0.

None of the three takes a lock, and the reason differs from object to object:
- `m_job` and `m_process` are set by the constructor and never reassigned, and the object they point to lives as long as they do.
- The channel is the interesting one. `m_peer`, the Arc to the other end, does change: `on_zero_handles()` empties it under the pair's shared lock when either end closes. So reading `m_peer->koid()` would need that lock, and could find `m_peer` empty.
- `m_peer_koid` is a plain number copied in `init_peer()`, before either end is reachable, and never written again. So it can be read with no lock, and it still answers after the peer has gone. The shell checks exactly that: it closes one end and reads the other's related koid again.

The worst interleaving the lock-free read has to survive is a close on another CPU in the middle of the get_info. With `m_peer_koid` that is harmless, because nothing writes it.

Rejected: option B, the field present but always 0 for now. It saves about twenty lines, but it would leave an ABI field whose 0 means either "no related object" or "not implemented yet", and a caller cannot tell which.

**Reference**
- Zircon `sys_object_get_info()`, case `ZX_INFO_HANDLE_BASIC` (`zircon/kernel/lib/syscalls/object.cc`): it requires no rights and fills `zx_info_handle_basic_t` from `handle->rights()`, `dispatcher->get_koid()`, `dispatcher->get_type()` and `dispatcher->get_related_koid()`. `InfoHandleBasic` keeps that layout: koid, rights, type, related_koid, then 8 bytes where Zircon has a reserved word and padding.
- Zircon `Dispatcher::get_related_koid()` and its overrides: `ProcessDispatcher` (the parent job), `ThreadDispatcher` (the process), and `PeeredDispatcher::get_related_koid()`, which returns `peer_koid_`, stored once by `InitPeer()`, for the same lock-free reason.

**Verification**
- Built and booted from `.worktrees/wip-proc-self` at 47940176 plus this change (the shared tree still holds another session's uncommitted VmoChildOptions change). `boot_check.py`: `dbg PASS 35 passed, 3 skipped, 0 failed`; `rel PASS 33 passed, 9 skipped, 0 failed, 6/6 required`.
- `shell: handle basic checks passed` on both images. That covers:
  - a process related to its job;
  - a thread related to its process;
  - each channel end related to the other, kept after the peer closed;
  - a root region and a job with related koid 0;
  - a handle without Inspect answered with its own rights;
  - BadHandle for an invalid handle.
- The same check confirms the ProcSelf rights the previous change could not: ProcSelf has the same rights as a handle fresh from `process_create`, on dbg_1 (handed over by userboot) and on dev (by the root's runner).
- `shell: object_get_info checks passed` on dbg, with topic 4 now the one answered NotSupported.

**Deliberately not done**
- A job's related koid, its parent job: the user's option listed three objects. A job answers 0 until something needs its parent.
- Comparing the exception tasks' koids with `ExceptionInfo`'s in `check_exception_tasks()`: now possible, but it belongs to slice 2, step 1, of `.agents/tasks/exception-channels/`.
- The profile program's MODULE line, which is what this was for: the next sub-step.

**How it was found**
None.

**The user's prediction**
None. The user chose option A (related koid filled for process, thread and channel now) over B (the field zero for now).

**Three questions you should be able to answer**
- Why does HandleBasic need no right when Process needs Inspect, and why could only the kernel have answered it at all?
- A channel end's related koid is read while another CPU may be closing the peer. Why is reading `m_peer_koid` safe without the pair's lock, when reading through `m_peer` would not be?
- After one end of a channel closes, what does the other end's HandleBasic report as related_koid, and why is that the useful answer?

## Title: kernel/object keeps no bare bool: a port's zero-handles bit in a Flags word, a channel's peer-closed read from its signal, and every "guarded by" comment a TA_GUARDED (2026-10-07)

**Files**
- `kernel/object/include/ours/object/port_dispatcher.hpp` — `bool m_zero_handles` replaced by `Flags m_flags TA_GUARDED(m_port_lock)`, a private `enum class Flags { None, ZeroHandles }`; the class comment says so
- `kernel/object/port_dispatcher.cpp` — the four reads test `Flags::ZeroHandles`, and `on_zero_handles()` sets it
- `kernel/object/include/ours/object/channel_dispatcher.hpp` — `bool m_peer_has_closed` removed; `peer_has_closed()` reads this end's `PeerClosed` signal; `m_messages` and `m_message_count` carry `TA_GUARDED(peer_lock())`, and `write_self_locked()` `TA_REQUIRES(peer_lock())`
- `kernel/object/channel_dispatcher.cpp` — `read()` asks `peer_has_closed()`; `write()` calls `peer->peer_lock().assert_held()` before handing the message to the peer
- `kernel/object/include/ours/object/dispatcher.hpp` — `PeerHolder::lock()` and `PeeredDispatcher::peer_lock()` carry `TA_RETURN_CAPABILITY`, so the analysis can follow `peer_lock()` to the holder's mutex
- `kernel/object/include/ours/object/exceptionate.hpp` — `m_channel`, `m_thread_rights` and `m_process_rights` carry `TA_GUARDED(m_lock)` in place of a comment; `has_valid_channel_locked()` `TA_REQUIRES(m_lock)`
- `kernel/object/exceptionate.cpp` — `send()` names the channel before its lambda, which the analysis checks as a function of its own
- `kernel/object/include/ours/object/observer.hpp` — `on_match()`'s comment says its argument is the object's whole signal state, not the triggering signals that matched
- `kernel/object/wait.cpp` — `WaitSignalObserver::m_cancelled` says why it stays an atomic bool
- `kernel/object/include/ours/object/exception_dispatcher.hpp`, `process_dispatcher.hpp`, `thread_dispatcher.hpp` — the nine members commented "Guarded by get_lock()" carry `TA_GUARDED(get_lock())`: `ExceptionDispatcher::m_state`, `m_thread_rights` and `m_process_rights`; `ProcessDispatcher::m_state`, `m_retcode`, `m_start_time` and `m_thread_list`; `ThreadDispatcher::m_state` and `m_kernel_thread`

**Symptom**
Latent, cannot be triggered today. Three kinds of drift from the style rules, none of which changed behaviour:

- two `bool` members in kernel objects, `PortDispatcher::m_zero_handles` and `ChannelDispatcher::m_peer_has_closed`, against the rule that an object's yes/no facts live in a Flags word or are not stored twice;
- fourteen members whose only protection was a comment, "Guarded by m_lock", "Guarded by peer_lock()" or "Guarded by get_lock()": an access without the lock would have compiled;
- `SignalObserver::on_match()` was documented as "called when the set of active signals matches an expected set", which leaves open whether its argument is the whole state or the part that matched. Every caller passes the whole state.

**Cause**
The rule of 2026-10-03 (modern-cpp-development) asks for three things, in order, of every `bool` member: derive it if it restates a fact the object already holds; otherwise put it in the class's Flags word, every bit of which shares one protection; keep a lone `bool` only with a comment saying why.

- `m_peer_has_closed` restated a fact. `on_peer_zero_handles_locked()` set it and, in the next statement, raised this end's `PeerClosed` signal. Both happen inside the one critical section of the holder lock, and nothing ever clears `PeerClosed` (it is set in that one place and no `update_state()` call clears it). So, read under the holder lock, as `read()` reads it, the signal and the `bool` could never disagree: two stores of one fact, the kind of duplicate that later drifts when one writer forgets the other. The signal is the one user space sees, so it is the one to keep.
- `m_zero_handles` is a state of the port, the same kind of fact as an exceptionate's shutdown: set once by `on_zero_handles()`, never cleared, and checked by every path that would queue or register afterwards. It could not be derived from the dispatcher's handle count, which reaches zero before `on_zero_handles()` takes `m_port_lock`; the bit is written under that lock precisely so that a callback that takes the lock after the tear-down sees it. So it goes in a Flags word guarded by `m_port_lock`, and the next yes/no fact the port needs has its place.

A comment saying "guarded by" is documentation the compiler cannot check. `TA_GUARDED` makes the thread-safety analysis, which is an error in this build (`-Wthread-safety`, `-Werror`), refuse an unlocked access. Annotating the channel's queue needed one more step: the queue is guarded by `peer_lock()`, a function that returns the mutex of a `PeerHolder` both ends share. Without `TA_RETURN_CAPABILITY` the analysis treats `peer_lock()` as an opaque expression, so `Guard guard{peer_lock()}` and `TA_GUARDED(peer_lock())` would only match by spelling, and `-Wno-thread-safety-attributes` would hide an attribute it could not resolve. With it, both resolve to `m_holder->m_lock`.

Two accesses then needed saying what the analysis cannot see:

- `ChannelDispatcher::write()` takes its own end's `peer_lock()` and then calls `peer->write_self_locked()`, which needs the peer's `peer_lock()`. They are one mutex, since both ends hold the same holder, but the analysis compares `this->m_holder->m_lock` with `peer->m_holder->m_lock` and finds two. `peer->peer_lock().assert_held()` tells it so, and a debug build checks that the claim is true.
- `Exceptionate::send()` wrote the message from inside a lambda that runs under `m_lock`. The analysis checks a lambda body as a separate function holding no lock, and reported `m_channel`. The channel is now named before the lambda, under the lock, and the lambda uses that name.

The analysis reported nothing else, so no access was racing.

**Change**
The port, before:

```cpp
    bool m_zero_handles TA_GUARDED(m_port_lock) = false;
```

After:

```cpp
    enum class Flags : u8 {
        None,
        ZeroHandles = 0_bit,
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(Flags);

    Flags m_flags TA_GUARDED(m_port_lock) = Flags::None;
```

The channel, before:

```cpp
    /// Messages the peer wrote and this end has not read yet. Guarded by
    /// peer_lock().
    MessagePacket::List m_messages;
    u32 m_message_count = 0;

    /// Set when the peer loses its last handle, never cleared. Guarded by
    /// peer_lock().
    bool m_peer_has_closed = false;
```

After:

```cpp
    [[nodiscard]]
    auto peer_has_closed() const -> bool {
        return !!(signals() & Signals::PeerClosed);
    }

    MessagePacket::List m_messages TA_GUARDED(peer_lock());
    u32 m_message_count TA_GUARDED(peer_lock()) = 0;
```

with `TA_RETURN_CAPABILITY(m_lock)` on `PeerHolder::lock()` and `TA_RETURN_CAPABILITY(m_holder->lock())` on `peer_lock()`.

The exceptionate, before and after:

```cpp
    /// Guarded by m_lock.
    KernelHandle<ChannelDispatcher> m_channel;
    ...
    /// See set_channel(). Guarded by m_lock.
    Rights m_thread_rights = Rights::None;
    Rights m_process_rights = Rights::None;
```

```cpp
    KernelHandle<ChannelDispatcher> m_channel TA_GUARDED(m_lock);
    ...
    /// See set_channel().
    Rights m_thread_rights TA_GUARDED(m_lock) = Rights::None;
    Rights m_process_rights TA_GUARDED(m_lock) = Rights::None;
```

The exception, process and thread dispatchers' members guarded by their object's own lock take the annotation with no other change: every access the analysis checks already holds `Guard guard{get_lock()}`, so the build stayed clean. For example:

```cpp
    /// The code the process ends with; see kill() and exit(). Read by
    /// get_info().
    i64 m_retcode TA_GUARDED(get_lock());
```

`WaitSignalObserver::m_cancelled` stays a `std::atomic<bool>`: it is the observer's only yes/no fact, and its writer runs under the observed object's lock, which the observer cannot name, so it has neither a word to share nor a lock to annotate.

**Reference**
Zircon keeps all of these as plain `bool`s: `PortDispatcher::zero_handles_ TA_GUARDED(get_lock())`, `ChannelDispatcher::peer_has_closed_ TA_GUARDED(channel_lock_)`, and in `Exceptionate`, `channel_handle_`, `thread_rights_` and `process_rights_` `TA_GUARDED(lock_)` (`kernel/object/include/object/*.h`). The Flags word and the derived peer-closed are this project's own rule. `SignalObserver::OnMatch()` carries the comment ours had, and `Dispatcher::AddObserver()` and `UpdateStateLocked()` pass it the whole active state, as ours do.

**Verification**
- The build of `out/bringup.x86` is clean with `-Werror -Wthread-safety`.
- The annotations are enforced, not silently dropped: with the `Guard` removed from `ChannelDispatcher::read()` and the `assert_held()` removed from `write()`, the build failed with `reading variable 'm_messages' requires holding Mutex 'm_holder->m_lock'` and `calling function 'write_self_locked' requires holding Mutex 'peer->m_holder->m_lock'`; the file was then restored. Likewise with the `Guard` removed from `ProcessDispatcher::get_info()`, from `ExceptionDispatcher`'s first locked function and from `ThreadDispatcher`'s, which failed with `reading variable 'm_state' requires holding Mutex 'get_lock()'` and the same for `m_retcode`, `m_start_time` and `m_kernel_thread`.
- `scripts/format_cpp.py` on every changed file; `clang-tidy` reports nothing on the changed lines (one pre-existing `modernize-use-override` at `dispatcher.hpp:177`).
- `boot_check.py`: dbg PASS (33 passed, 3 skipped, 0 failed), rel PASS (31 passed, 9 skipped, 6/6 required, `pc-ps2: key a` and `key A` present).
- `kshell_run.py port.test` on dbg: ok, 200 rounds, 3355 observers torn down, 145 callbacks yielded to the tear-down, 0 bad rounds.

**Deliberately not done**
- `SignalObserver::m_handle` and `m_triggering_signals` say "guarded by KernelObject's lock". That lock belongs to whichever dispatcher the observer is attached to, which the observer does not name, so there is no expression to put in `TA_GUARDED`.
- `PortObserver::m_object` is read and written under the port's lock, but it is in another class than the one with the `bool`.
- `PeeredDispatcher::m_peer` is read under `peer_lock()`, as its accessor says. Annotating it fails the build three times, each a true access the analysis cannot see: `init_peer()` writes it before either end is reachable, and `on_zero_handles()` clears the peer's link under the lock both ends share, which the analysis names `peer->m_holder->m_lock`. Each needs its own `assert_held()` or a stated exemption, which is more than an annotation.
- `Handle::m_table_hook` is guarded by the lock of whichever table holds the handle, which the handle does not name.
- `WaitSignalObserver::m_cancelled` could be a plain member: every read follows `detach_observer()`, which takes the lock `on_cancel()` ran under. Changing an atomic to a plain field is a synchronisation decision, not a style one, so it is left as it is.

**How it was found**
`docs/repairs/261006/cpp-style.md` item 1, which listed the two `bool`s and the exceptionate's unannotated members.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why can `read()` rely on the `PeerClosed` signal under the holder lock, although the signal itself is written under the end's own Dispatcher lock?
- Why could `m_zero_handles` not be derived from the dispatcher's handle count, when `m_peer_has_closed` could be derived from a signal?
- Two ends of one channel share one mutex. Why did the analysis still need `assert_held()` in `write()`, and what does the debug build check there?
