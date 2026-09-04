# TA_* 线程安全注解 — 待修

范围：`kernel/include/ours/**`、`kernel/kernel/**`、`kernel/mm/**` 里所有 `TA_*` 注解，以及
它们依赖的 `lockdep::Guard`、`sync::SpinLock`、`sync::Mutex`。历史见
`docs/repairs-20260830.md` 第十三节（首轮 347 条那次）。

判断依据是重编相关 15 个 TU 的实际告警（`-Wthread-safety` 已开、`-Wno-error`）：
基线 **219 条**；第 1 条修完 **194**，第 2 条修完 **187**，3.1 修完 **180**，3.6 修完 **178**，3.2 修完 **174**，4.3 修完 **172**，4.2 修完 **127**，3.3 修完 **123**，3.4 修完 **117**，3.5 修完 **113**，4.5 修完 **108**。

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

### 3.2 `Scheduler::unpark_thread(Thread *)` 契约 / `suspend_or_kill` 锁泄漏 — 已修

178 → 174，见 changes。

### 3.3 `do_unpark_thread` / `process_migrating_threads` / `unpark_common` — 已修

127 → 123。

### 3.4 上下文切换的锁交接 — 已修

123 → 117。

### 3.5 `Scheduler::set_state` — 已修

117 → 113。

### 3.6 `Timer::trylock_or_cancel` — 已修

180 → 178，见 changes。

### 3.7 遗留拼法 — 已修

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

### 4.2 `PmNode` 三个 CpuMask — 已修

172 → 127。

### 4.3 `MemCache::SLAB_NODES` — 已修

174 → 172。

### 4.4 `WaitQueue` 三处调用方缺传播

`WaiterState::sleep()`/`unsleep()`（`wait_queue.cpp:9-19`）和 `park_thread_common()`（`:79`）
调 `Scheduler::Current::park_thread`/`unpark_thread`（要求 pi 锁）但自己没标。`enqueue_waiter`
的注释已经写了「调用方持 waiter 的 pi 锁」，把它变成 `TA_REQUIRES(thread->get_pi_lock())`。

### 4.5 `Scheduler::Migrator::move_queued_thread` — 已修

113 → 108。

### 4.6 `finish_transition` 从不持 PI 锁的 lambda 里被调

`scheduler.cpp` `process_migrating_threads()` 通过 `find_active_scheduler_for(to_migrate, [](Thread
*thread, Scheduler *self) { self->finish_transition(thread); })` 调它，lambda 里只持目标调度器的
rq 锁，不持 `thread->get_pi_lock()`，而 `finish_transition` 标了 `TA_REQUIRES(thread->get_pi_lock())`。
函数体目前是只有断言的桩，契约该是什么等实现定；归调度器迁移工作。

### 4.7 唤醒路径不持被唤醒线程的 PI 锁 — 已修

见 changes。**未实测**（启动走不到）。

## 5. 已确认正确、不用动的

修完第 1 条后这些告警全部消失，说明注解本身写对了：`TimerQueue`（`m_lock` 及六个
`*_locked()`）、`ClockSource::REGISTERED_SOURCES*`、`Dpc::Queue::m_list`、`Event` 对
`lock_for_notify_*` 的调用、`MemCache::GLOBAL_CACHES`。`TimerQueue::with_queue_locked` 和
`Timer::cancel` 里 lambda 上的 `TA_NO_THREAD_SAFETY_ANALYSIS` 是合理的：分析器跟不过回调边界。
