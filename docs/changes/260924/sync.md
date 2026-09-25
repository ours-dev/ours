# sync 改动记录（2026-09-24）

## `Mutex::release_slowpath()` 真的唤醒等待者（2026-09-24）

**文件**
- `kernel/kernel/task/owned_wait_queue.cpp` — `lock_for_notify_operation()` 从空桩改成真正出队；
  `notify_threads_locked()` 改名 `notify_threads()` 并真正唤醒
- `kernel/include/ours/task/owned_wait_queue.hpp` — 两个函数的锁约定写进声明；公开 `get_lock()`
- `kernel/kernel/sync/mutex.cpp` — `release_slowpath()` 在队列锁下出队并发布新状态，放锁后再唤醒

**症状**
任何一次 `sync::Mutex` 争用都让输掉的那个线程永远睡下去。dbg_1 镜像上 P1-A 的探针测试
约一半的启动停在 "DBG detaching from koid 31"：shell 在 `Dispatcher::detach_observer()` 里
等通道 dispatcher 的锁，QEMU monitor 看它不在任何运行队列上。

**原因**
唤醒的那一半根本没写。`OwnedWaitQueue::lock_for_notify_operation()` 是个桩，无条件返回 none：

```cpp
template <typename UnparkThreads>
auto OwnedWaitQueue::lock_for_notify_operation(usize max_notify, INotifyRequeueHook &)
    -> ustl::Option<UnparkThreads> {
    return ustl::NONE;
}

template <typename UnparkThreads>
auto OwnedWaitQueue::notify_threads_locked(UnparkThreads, INotifyRequeueHook &, NotifyOptions)
    -> void {}
```

它写于 `WaitQueue::lock_for_notify_one/all` 也还是桩的时候。后来那两个为 `Event` 补上了，这个没有。
于是 `release_slowpath()` 永远"找不到人交接"，把锁放成 `Available` 就走；而 `acquire_slowpath()`
自 `e0cbf29a` 起是真的 park 了——park 进去的线程再没有人叫醒。

为什么是"任何争用"而不只是某个竞态窗口：快路径存进 `m_state` 的总是 `thread | Contended`
（见下一条要修的），所以慢路径里 CAS 失败后看到的旧值必然带 `Contended`，自旋循环第一圈就 break，
直奔 park。争用一次，丢一个线程。

报告里描述的那个竞态（等待者 CAS 失败后、入队前，持有者发布 `Available`）是真的，但它叠在这个
更大的洞上面：在这一条之前，即便等待者赶在释放之前入了队，也照样没人叫它。那一条是下一节。

**改法**
照 Zircon 把唤醒拆成两半，但锁的放法按本树的锁序改：

```cpp
// release_slowpath()
task::AutoPreemptDisabler const preempt_disabler;
ustl::Option<Thread::UnparkList> batch;
{
    Guard<SpinLock, IrqSave> guard{m_waiters.get_lock()};
    batch = m_waiters.lock_for_notify_operation<Thread::UnparkList>(1, default_hook);
    if (batch) {
        publish(new_owner | State::Contended);
    } else {
        publish(State::Available);
    }
}
if (batch) {
    m_waiters.notify_threads(ustl::move(*batch), default_hook, NotifyOptions::None);
}
```

- **出队和发布在同一把队列锁下**。等待者入队要拿这把锁，所以"队列为空"这件事在 `Available`
  写进 `m_state` 之前不会变。这是下一节等待者复查能成立的前提。
- **唤醒在放锁之后**。唤醒要拿被唤醒线程的 PI 锁，本树里 PI 锁在外、队列锁在内（见
  `WaitQueue::enqueue_waiter` 的注释），在队列锁下拿 PI 锁就是反序。Zircon 的
  `WakeThreadsLocked()` 能在队列锁下唤醒，是因为它的 ChainLock 事务已经把所有相关锁都拿着了；
  本树没有那个协议。所以名字去掉 `_locked`——留着就是在说谎。
- **从发布到唤醒关抢占**。这段时间里锁的主人是一个还没跑起来的线程；若释放者在中间被抢占，
  锁就挂在一个睡着的主人名下，其他竞争者全排在后面。Zircon `ReleaseContendedMutex()` 同样关抢占。
- 队列锁 `IrqSave`：`WaitQueue::handle_timeout` 会在中断上下文里拿队列锁。今天没人带 deadline
  在 mutex 上 park，但队列本身并不知道这一点。
- `notify_threads()` 复用 `WaitQueue::wake_dequeued_thread()`，它顺带做 PI 边的
  `REMOVE_SINGLE_EDGE` 传播，和 `notify_one()` 同一条路。

被否掉的方案：让 `lock_for_notify_operation()` 自己拿队列锁、`notify_threads_locked()` 放锁
（Zircon 的形状）。那样发布 `m_state` 就要夹在两个函数之间、持着一把别人拿的锁，读代码时看不出
锁的边界；而且和 `WaitQueue::lock_for_notify_one()`"调用者持锁"的约定相反。

**参照**
- Zircon `Mutex::ReleaseContendedMutex()`（`kernel/kernel/mutex.cc`）：
  `LockForWakeOperationOrBackoff(1u, ...)` → CAS 发布新状态 → `WakeThreadsLocked(...)`。
- Zircon `OwnedWaitQueue::LockForWakeOperationOrBackoff()`、`WakeThreadsLocked()`。
- 本树 `Event::signal()`：同样是"队列锁下 `lock_for_notify_*`、放锁后 `unpark`"。

**验证**
- `prebuilt/ninja/ninja -C out/bringup.x86` 通过；clang-format 无残留；clang-tidy 在改动行上无新诊断。
- dev 镜像启动：`init: shell says "pong"`、`shell: channel checks passed`、
  `shell: bad-pointer checks passed` 都在。
- dbg_1 镜像启动：`shell: channel checks passed`、`shell: bad-pointer checks passed` 都在。
- P1-A 的探针测试不在树里（暂存在会话外），这一步没有跑它。这一步只补了唤醒，竞态窗口还在，
  下一节补。

**故意没做**
- 竞态窗口（等待者在队列锁外判断要不要 park）——下一节，单独一步。
- 优先级继承：`notify_threads()` 不把队列的 owner 转给新主人。PI 的加锁协议还是占位符，
  见 `docs/repairs/260915/sync-mutex.md` 第 1 条。
- `~Mutex` 不等队列锁放掉：发布 `Available` 之后、放队列锁之前，别的线程可以走快路径拿锁、放锁、
  销毁 mutex，放锁那一下就写到了已释放的内存。Zircon 在 `Mutex::~Mutex()` 里自旋等 OWQ 锁放开来
  挡这个。本树的 `Mutex` 没有析构函数，另记待修。

## `Mutex` 的等待者在队列锁下复查状态再 park（2026-09-24）

**文件**
- `kernel/kernel/sync/mutex.cpp` — `acquire_slowpath()` 自旋结束后，持 PI 锁 + 队列锁复查 `m_state`，
  决定"拿下"还是"入队"，入队用新的两段式 park
- `kernel/include/ours/task/owned_wait_queue.hpp`、`kernel/kernel/task/owned_wait_queue.cpp` —
  新增 `park_and_assign_owner_preamble_locked()` / `park_and_assign_owner_postamble()`

**症状**
上一节补上唤醒之后仍会丢唤醒：等待者 CAS 失败之后、入队之前，持有者若恰好释放，
等待者就睡在一把没人持有的锁上。dbg_1 探针测试里表现为 shell 停在 `detach_observer()`。

**原因**
"要不要睡"和"入队"不在同一把锁下。旧代码：

```cpp
// 自旋循环里最后一次 CAS 失败，就是最后一次看 m_state
auto const status = m_waiters.park_and_assign_owner(current_thread, time::Deadline::infinite(),
                                                    task::Interruptible::No);
```

`park_and_assign_owner()` 先拿 PI 锁，再在 `enqueue_waiter()` 里自己拿、自己放队列锁。
释放者 `release_slowpath()` 在队列锁下看队列：此刻等待者还没入队，队列为空，于是发布 `Available`。
这之后等待者入队、park。它等的是一次"释放"，而那次释放已经发生过了——释放只发生一次，
错过就是永远错过。条件变量的经典丢唤醒，一模一样：检查条件和进入等待之间必须没有缝。

堵缝的办法只有一个：让"看条件"和"入队"在释放者也必须拿的那把锁下连成一步。
这样释放要么落在检查之前（等待者看见 `Available`，直接拿下），要么落在入队之后
（释放者看见等待者，交接给它）。

**改法**

```cpp
Guard<SpinLock, IrqSaveAndPreemptDisable> pi_guard{current_thread->get_pi_lock()};
{
    Guard queue_guard{m_waiters.get_lock()};
    auto old_mutex_state = m_state.load(std::memory_order_relaxed);
    while (!(old_mutex_state & State::Contended)) {
        if (State::Available == old_mutex_state) {
            if (m_state.compare_exchange_weak(old_mutex_state, new_mutex_state, ...)) {
                record_initial_assigned_cpu();
                return false;                        // 释放已经发生：直接拿下
            }
        } else if (m_state.compare_exchange_weak(old_mutex_state,
                                                 old_mutex_state | State::Contended, ...)) {
            break;                                   // 有主：打上 Contended，逼它走慢路径
        }
    }
    status = m_waiters.park_and_assign_owner_preamble_locked(current_thread, ...);  // 入队
}
status = m_waiters.park_and_assign_owner_postamble(current_thread, ...);            // 睡
```

- 锁序 PI 锁在外、队列锁在内，和 `WaitQueue::enqueue_waiter` 一致；PI 锁要关中断关抢占，
  因为后半段把它带进 `reschedule_common()`。形状与 `Event::wait_etc()` 相同。
- `Contended` 一旦在 `m_state` 里，只有持有者的 `release_slowpath()` 在队列锁下能把它拿掉；
  所以"看到 `Available`"意味着队列必空（队列里有人必有 `Contended`），直接拿下是安全的。
- 用 CAS 循环而不是 Zircon 的 `fetch_or`：`State` 是枚举，`std::atomic<State>` 没有 `fetch_or`；
  顺带也不会像 `fetch_or` 那样先让别的 CPU 看到一个"无主但 Contended"的瞬态。循环只在 CAS 输给
  快路径的获取/释放时重来。
- `OwnedWaitQueue` 新增两段式 park，契约照抄 `WaitQueue::park_thread_preamble_locked()` /
  `park_thread_postamble()`。原来的 `park_and_assign_owner()` 保留不动（现在没有调用者）：
  它的注释说明了为什么等锁这类调用者不能用它。

被否掉的方案：在 `park_and_assign_owner()` 里加一个回调让它在队列锁下复查。那是把 mutex 的
状态机塞进等待队列；两段式把判断留在 mutex 自己手里，`Event` 已经是这么用的。

**参照**
- Zircon `Mutex::AcquireContendedMutex()`（`kernel/kernel/mutex.cc`）：拿到 OWQ 锁后
  `val_.fetch_or(STATE_FLAG_CONTESTED)`，旧值是 `STATE_FREE` 就直接拿下，否则
  `BlockAndAssignOwnerLocked()`。注释原话的意思：CONTESTED 只能在持 OWQ 锁时改。
- Linux `__mutex_lock_common()`：持 `wait_lock` 后先 `__mutex_trylock()` 再
  `__mutex_add_waiter()`，同一个"锁下先复查再入队"。
- 本树 `Event::wait_etc()` + `WaitQueue::park_thread_preamble_locked()`：同一类缝，先前已补过。

**验证**
- 构建通过；clang-format 无残留；clang-tidy 在改动行上无新诊断。
- dev 镜像：`init: shell says "pong"`、`shell: channel checks passed`、
  `shell: bad-pointer checks passed` 都在。dbg_1 镜像：后两行都在。
- 专门的争用压力测试见后续一节；P1-A 探针测试不在树里，未实测。

**故意没做**
- 快路径仍然存 `thread | Contended`，所以 `try_release()` 的快路径仍是死的——下一节单独改。
  注释里因此没有写"Contended 只在队列锁下设置"：这句话要到下一节才成立。
- 自旋阶段仍然不自旋（`deadline_ticks = now_ticks`），另起一节。
- 递归获取（持有者再 `acquire()` 自己）会在这里永远 park。Zircon 在阻塞前
  `if (LK_DEBUGLEVEL > 0 && IsHeld()) panic(...)`；本树还没有，另记待修。

## `mutex.stress`：专测 `Mutex` 丢唤醒的控制台命令（2026-09-24）

**文件**
- `kernel/commands/mutex_stress.cpp` — 新增 `mutex.stress [rounds]`
- `kernel/commands/BUILD.gn` — 加入源文件，依赖 `//system/lib/lockdep`（`Guard`）

**症状**
无（新增测试）。前两节修的丢唤醒，正常启动几乎碰不到——要靠 P1-A 探针测试一个接一个杀进程，
才在约一半的启动里撞上一次。没有一个能按需复现它的东西，就说不清修好没有。

**原因**
丢唤醒的窗口是"等待者 CAS 失败"到"等待者入队"之间那几条指令，只有释放恰好落在里面才会出事。
要稳定撞上，得同时满足：有人在锁里待得够久，让别人来得及 CAS 失败并走向 park；
又有人释放得够频繁，让释放有机会落进那个窗口。

**改法**
三个线程钉在三个不同的 CPU 上（真并行，不是时间片轮转），抢同一把 `sync::Mutex`：
两个在锁里 `spin_hint()` 64 次（制造"来得及失败"），一个拿了就放（制造"频繁释放"）。

```cpp
for (usize i = 0; i < state.rounds; ++i) {
    Guard guard{state.mutex};
    if (state.mutex.holder() != self) { ++violations; }   // holder() 必须是自己
    auto const before = state.counter;                     // 读
    for (usize k = 0; k < worker.hold_spins; ++k) { arch::spin_hint(); }
    state.counter = before + 1;                            // 写：两个线程同时在里面就会丢增量
}
state.finished.fetch_add(1, std::memory_order_release);
```

判定：
- 丢唤醒 = 有线程永远回不来。命令线程按**时钟**轮询完成计数，20 秒没到齐就报
  `STUCK -- k/3 workers finished ..., holder X`。`holder 0x0` 就是"睡在一把没人持有的锁上"。
- 互斥被破坏 = 计数器丢增量或 `holder()` 不对，报 `FAILED`。
- 都没有，报 `ok -- N rounds x 3 threads in T us`。

几处取舍：
- 做成控制台命令而不是单元测试：树里还没有内核内的 unittest 运行时（`vmours.with-tests`
  的依赖图还是空的），而 mutex 的慢路径就是调度器，没法在主机上测。按树的约定，
  只为控制台存在的文件放 `kernel/commands`。
- 超时按时钟而不是按轮询次数：第一版数 3000 次 `sleep_for(10ms)`，实测 30000 ms 的"超时"
  花了约 118 秒——每次睡眠都被节拍向上取整。
- 状态放堆上：失败时卡住的线程还指着它，只能泄漏，不能随栈弹掉。

**参照**
无直接对应。Zircon 的内核内单元测试（`k ut` 运行）里，`kernel/tests/pi_tests.cc` 让多个线程阻塞在
`OwnedWaitQueue` 上、再验证唤醒与优先级继承（不经过 `Mutex`），`kernel/tests/mutex_spin_time_tests.cc` 测自旋阶段；
本树没有那套运行时，所以落成控制台命令。

**验证**
- 构建通过；clang-format 无残留；clang-tidy 在新文件上无诊断。dev、dbg_1 两个镜像启动回归通过。
- **目前从 kshell 敲不了**：任何 kshell 命令第一次分派都会 panic，因为 `gktl::run_command()` 的
  命令树用 `std::allocator`，落到裸 `operator new` 的 panic 桩上。那是 gktl 的问题，已交接给独立会话。
  下表是用一个**只在本地、未提交**的启动钩子直接调用本命令跑出来的（每次启动连跑 5 轮，
  每轮 20000 次 × 3 线程），修复前的两个版本在隔离的 worktree 里构建，没有动共享工作区：

  | mutex 版本 | 启动次数 | 轮数 | STUCK | FAILED | 一轮耗时 |
  |---|---|---|---|---|---|
  | 修复前（`c4f4300b`） | 1 | 1 | 1（1/3 回来，holder 0x0） | 0 | — |
  | 只补唤醒（`4bf196fa`） | 3 | 15 | 4（均 2/3 回来，holder 0x0） | 0 | 2.2–2.9 s |
  | 补上复查（`4b70d646`） | 4 | 20 | 0 | 0 | 2.3–2.9 s |

  修复前那一行只跑出一轮：第一轮就卡住，之后的轮次在超时前没来得及跑完（轮询按次数计时的旧版本）。
  "只补唤醒"那一行正是报告里的竞态：锁已空（holder 0x0），有一个线程睡在队列里。

**故意没做**
- 不在启动时自动跑：它会给每次 dbg 启动加几秒，而且现在只有通过启动钩子才能跑起来，
  那个钩子是验证用的，不属于内核。
- 没有测 `CriticalMutex`（时间片延长那条路）：那个类现在是空的。
