# TA_* 线程安全注解 — 待修

范围：`kernel/include/ours/**`、`kernel/kernel/**`、`kernel/mm/**` 里所有 `TA_*` 注解，以及
它们依赖的 `lockdep::Guard`、`sync::SpinLock`、`sync::Mutex`。历史见
`docs/repairs-20260830.md` 第十三节（首轮 347 条那次）。

判断依据是重编相关 15 个 TU 的实际告警（`-Wthread-safety` 已开、`-Wno-error`）：
基线 **219 条**；第 1 条修完 **194**，第 2 条修完 **187**，3.1 修完 **180**，3.6 修完 **178**，3.2 修完 **174**，4.3 修完 **172**，4.2 修完 **127**，3.3 修完 **123**，3.4 修完 **117**，3.5 修完 **113**，4.5 修完 **108**，4.1 修完 **24**。

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

### 4.1 `Thread::m_flags` / `m_scheduling_context` — 已修（108 → 24）

决定：`m_flags` 归 PI 锁；`m_scheduling_context` 归所在 runqueue 的锁（Linux 模型），因为写不出
capability 表达式而不标注解。见 changes。

### 4.1b `Thread::m_stack` / `m_arch_thread` / `m_task_state` 仍标 `TA_GUARDED(get_resource_lock())`

剩余 24 条里的 18 条。`kernel_stack()` 按引用返回 `m_stack`（11 TU 各一条）；`spawn_etc`/
`init_first` 构造期裸写三者（`thread.cpp:118-151`）；`join()`/`exit()`/`trampoline()` 不持锁读写
`m_task_state`（`:303/310/396/653/663`）。要先定：

- `m_stack`、`m_arch_thread`：构造期写一次，之后只有上下文切换（`ArchThread::switch_thread`）
  和 `mp.cpp`/`x86 thread.cpp` 读栈顶。建议去掉 `TA_GUARDED`，注释「发布前写、之后只读」。
- `m_task_state`：join 与 exit 之间的握手，内部自带 wait queue。要么它自己就是同步点、去掉
  注解；要么 join/exit 真的在 resource 锁下访问它。看 `TaskState` 的实现再定。

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
