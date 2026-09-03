# TA_* 线程安全注解 — 待修

范围：`kernel/include/ours/**`、`kernel/kernel/**`、`kernel/mm/**` 里所有 `TA_*` 注解，以及
它们依赖的 `lockdep::Guard`、`sync::SpinLock`、`sync::Mutex`。历史见
`docs/repairs-20260830.md` 第十三节（首轮 347 条那次）。

判断依据是重编相关 15 个 TU 的实际告警（`-Wthread-safety` 已开、`-Wno-error`）：
基线 **219 条**；第 1 条修完 **194**，第 2 条修完 **187**，3.1 修完 **180**。

按严重程度排。前两条是根因，不修它们，后面的每一条都分不清是注解错还是分析器看不见。
前两条已修；`Guard` 的注解在子模块路径下提交不进来，只在工作区，重新 checkout 会丢。

---

## 1. 根因一：Guard 和锁类对分析器不可见 — 已修

见 `docs/changes/260903/thread-safety.md`。`ScopedGuard` 仍对分析器不可见（属性参数不能展开
参数包），它的三个调用点（`scheduler.cpp:346/830`、`scheduler.hpp` `Current::set_state`）要换成
嵌套 `Guard`，归到下面 3.x 一起做。修完后 15 个 TU 的告警 219 → 194。

## 2. 根因二：`TA_REQUIRES(get_lock())` 和 `Guard guard{m_lock}` 对不上 — 已修

四个访问器加了 `TA_RETURN_CAPABILITY`，见 `docs/changes/260903/thread-safety.md`。194 → 187。
从这里起剩下的告警里锁名都是成员表达式，每一条都是注解或代码本身的问题。

## 3. 注解方向写反或语义错（这些不是「漏」，是「错」）

### 3.1 `reschedule_common` 的锁契约 / `yield_thread` 死锁 — 已修

见 changes。187 → 180。

### 3.2 `Scheduler::unpark_thread(Thread *)` — `TA_ACQUIRE(thread->get_pi_lock())`，实现既不拿也不放

`scheduler.cpp:187-203` 里没有任何对 pi 锁的操作，只调 `do_unpark_thread`。调用方现状：

- `thread.cpp:201/203`（`resume`）在 `Guard{get_pi_lock()}` 作用域内调 → 分析器报「重复获取」；
- `thread.cpp:245`（`suspend_or_kill`，Suspend + Kill 分支）先 `thread_guard.unguard()` 再调，
  即认为 unpark 会替它放锁——**而它不放**，这条路径上 pi 锁永远不会释放；
- `wait_queue.cpp:19`（`WaiterState::unsleep`）、`wait_queue.cpp:281`（`handle_timeout`，
  之后手动 `pi_lock.release()`）都按「调用方持有、调用方放」用。

三种用法里两种半是「调用方持锁并自己放」。定为 `TA_REQUIRES(thread->get_pi_lock())`
（Zircon `Scheduler::Unblock` 也是要求 thread_lock 已持有），并删掉 `thread.cpp:245` 的
`unguard()`。列表重载 `unpark_thread(UnparkList)` 自己 `Guard` 每个线程的 pi 锁，不需要注解。

### 3.3 `do_unpark_thread` / `process_migrating_threads` — 标 `TA_ACQUIRE`，函数体内是完整的 Guard 作用域

`scheduler.hpp:178` `TA_ACQUIRE(get_lock())`、`scheduler.hpp:199`
`TA_ACQUIRE(m_runtime.migrating_threads_lock)`。两处函数体都是 `Guard guard{...}` 拿了又放，
调用方出来时并不持锁（`scheduler.cpp:657/391` 报「期望出口持锁」）。`TA_ACQUIRE` 是给
「拿了不放、留给调用方」的函数用的（如 Linux `spin_lock` 本身）。这两处去掉注解，或改
`TA_EXCLUDES(...)` 声明「进来时别持有」。

`unpark_common`（`scheduler.hpp:142`，同样 `TA_ACQUIRE(get_lock())`）只有声明没有定义，
注解跟着一起改就行。

### 3.4 `switch_context` / `finish_switch_context` / `trampoline_handoff` — 上下文切换的锁交接

`switch_context` 标 `TA_RELEASE(get_lock())` 是对的（`scheduler.cpp:465`）；但它末尾调的
`finish_switch_context` **也**标了 `TA_RELEASE(get_lock())`，而后者放的是
`prev_thread->get_pi_lock()`（`scheduler.cpp:446`），不是 rq 锁——两次「释放 rq 锁」。
`trampoline_handoff`（`:106/108`）是同一交接的另一端。

这是分析器表达不了的模式（锁由切过去的线程放）。Zircon 的做法是把这段用
`__TA_NO_THREAD_SAFETY_ANALYSIS` 圈起来并在注释里写清交接协议。本树对应：
`finish_switch_context` 去掉错误的 `TA_RELEASE(get_lock())`，加
`TA_NO_THREAD_SAFETY_ANALYSIS`，注释写明它放的是 `m_runtime.prev_thread` 的 pi 锁、
为什么必须由 next 放。`trampoline_handoff` 同理。

### 3.5 `Scheduler::set_state` — `TA_REQUIRES(get_lock())` 保护不了它保护的东西

`scheduler.cpp:284-288`：对全局 `STATES[state]`（`Atomic<CpuMask>`）做 relaxed load → 改本 CPU
位 → relaxed store。本 CPU 的 rq 锁挡不住另一个 CPU 对同一个 mask 做同样的 RMW，两个 CPU
同时切 idle 会丢一位。注解在这里制造了「有锁保护」的错觉，还逼着
`Current::set_state`（`scheduler.hpp:270-276`）专门拿一次 rq 锁来满足它。

应该是无锁的原子 `fetch_or`/`fetch_and`（Zircon `mp.idle_cpus` 就是这么改的），注解删掉。
`CpuMask` 目前是否提供原子位操作要看 `ustl`，没有就补一个。

### 3.6 `Timer::trylock_or_cancel` 缺 `TA_TRY_ACQUIRE(false, lock)`

`timer.cpp:94-102`：返回 `false` 表示锁**已拿到**，`true` 表示放弃。分析器不知道，所以
`wait_queue.cpp:281` 的 `pi_lock.release()` 报「释放未持有」，`timer.cpp:102` 报「不是每条
路径都持锁」。标 `TA_TRY_ACQUIRE(false, lock)`（成功值是 false，和常规相反，注释要写明）。

### 3.7 遗留拼法 `TA_REQUIRE` / `TA_GUARD`

`scheduler.hpp:139`（1 处 `TA_REQUIRE`）、`object_cache.hpp:125-126`、`pm_node.hpp:367-371`
（5 处 `TA_GUARD`）。`compiler.hpp:97-99` 的兼容宏是为它们留的；改成 `TA_REQUIRES`/
`TA_GUARDED` 后把三个兼容宏一起删掉，免得两套拼法并存。`TA_CAP` 无人用，一并删。

顺带：`mutex.hpp:80` `class TA_CAPABILITY("Cirtical-Mutex") CirticalMutex` 拼错，应为
`Critical`。

## 4. `TA_GUARDED` 标了，但访问器没有 `TA_REQUIRES`（数量最多，需先做决定再补）

### 4.1 `Thread` 五个成员

`thread.hpp:396-416` 把 `m_arch_thread`、`m_stack`、`m_flags`、`m_task_state` 标 `resource_lock`，
`m_scheduling_context` 标 `pi_lock`，但读写它们的访问器全部裸着：`kernel_stack()`(:211)、
`last_cpu()`/`curr_cpu()`(:215/219)、`is_idle_thread()`(:255)、`scheduling_context()`(:330，
返回可写引用)、`set_flag_idle()`/`set_flag_nofree()`(:358/362)。这 7 个访问器在 11 个 TU 里
各报一次，合计 **121 条**，占剩余告警一半以上。

补之前要先回答两个问题，否则补出来的 `TA_REQUIRES` 是错的：

- **`m_flags` 到底归哪把锁？** `detach()`（`thread.cpp:300-315`）和 `Current::exit()`
  （`:586-590`）在 `pi_lock` 下读写它，`set_user_thread()`（`:336-339`）在 `resource_lock` 下写它，
  `join()`（`:279`）和 `destroy()`（`:352`，且在 `~Self()` 之后读）不拿任何锁。注解说
  `resource_lock`，代码多数用 `pi_lock`。Zircon 只有一把 `thread_lock`，所以没有这个问题；
  这里得先定下来。
- **`m_scheduling_context` 由 pi 锁保护，还是由所在 rq 锁保护？** 调度器代码
  （`set_schedule`/`get_schedule`/`evaluate_next_thread`/`enqueue_thread`……）在持 rq 锁不持
  pi 锁时大量读写它。Linux 的 `sched_entity` 归 `rq->lock`；Zircon 归 `thread_lock`。按现在的
  代码看更像 Linux 模型，那 `TA_GUARDED(get_pi_lock())` 就是错的。

`spawn_etc()`/`init_first()`（`thread.cpp:118-140`）在对象发布前裸写 `m_stack`/`m_task_state`/
`m_arch_thread`，这是构造期的正常做法，标 `TA_NO_THREAD_SAFETY_ANALYSIS` 并注明「未发布」。

### 4.2 `PmNode` 三个 CpuMask

`pm_node.hpp:300-308` 三个访问器把 `TA_GUARD(get_lock())` 的成员按 `const &` 返回，
`:421` 静态 `hard_affinity()` 也是；`pm_node.cpp:352` 在 bring-up 里裸写 `m_hard_affinity`。
它们是启动期写一次、之后只读的 mask，`m_work_cpus` 才真的在 `cpu_to_node` 变化时改
（`pm_node.cpp:762-813`，那三处是在 Guard 下的，正确）。

建议：`m_hard_affinity`/`m_soft_affinity` 去掉 `TA_GUARD`，注释写「只在 online 前写」；
`m_work_cpus` 保留，访问器改为持锁按值返回。

### 4.3 `MemCache::SLAB_NODES`

`object_cache.cpp:376`（分配路径）和 `:494`（`init()`）不持 `CACHE_LOCK` 读它。`:494` 是单线程
init；`:376` 在热路径上，要拿 Mutex 不合理。这是启动期定下的只读 nodemask，
`TA_GUARD(CACHE_LOCK)` 应去掉。`GLOBAL_CACHES` 的 Guard 是正确的（实验中告警消失）。

### 4.4 `WaitQueue` 三处调用方缺传播

`WaiterState::sleep()`/`unsleep()`（`wait_queue.cpp:9-19`）和 `park_thread_common()`（`:79`）
调 `Scheduler::Current::park_thread`/`unpark_thread`（要求 pi 锁）但自己没标。`enqueue_waiter`
的注释已经写了「调用方持 waiter 的 pi 锁」，把它变成 `TA_REQUIRES(thread->get_pi_lock())`。

### 4.5 `Scheduler::Migrator::move_queued_thread`

`scheduler_migrator.cpp:10-24` 裸 `release()` 本 CPU rq 锁、`acquire()` 目标 CPU rq 锁，
没有任何注解（`:16/24` 报）。前置条件是 `self->get_lock()` 已持有、出口持有的是**另一个**
scheduler 的锁——分析器表达不了，标 `TA_REQUIRES(self->get_lock())` +
`TA_NO_THREAD_SAFETY_ANALYSIS` 并写注释。

## 5. 已确认正确、不用动的

修完第 1 条后这些告警全部消失，说明注解本身写对了：`TimerQueue`（`m_lock` 及六个
`*_locked()`）、`ClockSource::REGISTERED_SOURCES*`、`Dpc::Queue::m_list`、`Event` 对
`lock_for_notify_*` 的调用、`MemCache::GLOBAL_CACHES`。`TimerQueue::with_queue_locked` 和
`Timer::cancel` 里 lambda 上的 `TA_NO_THREAD_SAFETY_ANALYSIS` 是合理的：分析器跟不过回调边界。
