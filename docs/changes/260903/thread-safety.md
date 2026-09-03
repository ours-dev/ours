# TA_* 线程安全注解 — 改动记录

待修清单在 `docs/repairs/260903/thread-safety.md`；审计方法和基线数字（15 个 TU、219 条）也在那里。

---

## Guard 和三个锁类补上分析器需要的注解（2026-09-03）

**文件**
- `system/lib/lockdep/include/lockdep/guard.hpp` — `Guard` 标 `TA_SCOPED_CAPABILITY`，构造
  `TA_ACQUIRE(lock)`，析构/`unlock()`/`unguard()` 标 `TA_RELEASE()`；真正碰锁的两个私有 helper
  和 `ScopedGuard` 的实现标 `TA_NO_THREAD_SAFETY_ANALYSIS`。**这个文件在 `system/lib/lockdep`
  子模块路径下，从本 worktree 提交不进主仓，改动只在工作区。**
- `kernel/include/ours/sync/spin_lock.hpp` — `acquire()`/`acquire_irq_save()` 标 `TA_ACQUIRE()`，
  `try_acquire()` 标 `TA_TRY_ACQUIRE(true)`，`release()`/`release_irq_restore()` 标 `TA_RELEASE()`，
  `assert_held()` 标 `TA_ASSERT()`。
- `kernel/include/ours/sync/mutex.hpp` — `acquire()`/`release()`/`assert_held()` 同上；
  `assert_held()` 顺带加 `[[nodiscard]]`（clang-tidy 对改动行的要求，它返回 bool）。
- `kernel/include/ours/sync/local_lock.hpp` — `acquire()`/`release()` 同上。

**症状**
潜伏。`-Wthread-safety` 已开，但全树所有 `TA_REQUIRES` 都报「未持锁」、所有 `TA_RELEASE` 都报
「释放未持有的锁」，真告警淹在假告警里，没人看。

**原因**
clang 的分析只认注解，不读实现。一把锁的 `acquire()` 没标 `TA_ACQUIRE()`，分析器就不知道调它
会持有 capability；`Guard` 不标 `TA_SCOPED_CAPABILITY`，RAII 作用域内的持锁事实同样不存在。
于是自 `61af5d0` 打开分析以来，Scheduler/WaitQueue/TimerQueue 上几十条 `TA_REQUIRES` 全部
没有任何一次能被满足——注解写了两年，分析器一次都没真正检查过它们。

`Guard` 内部为什么要把 `guard()`/`release_lock()` 藏起来：scoped capability 的模型是「Guard 对象
本身是一个 capability」，构造 = 获得 `*this`，析构 = 释放 `*this`。如果分析器同时也跟着
`m_lock.acquire()` 走，同一把锁会被记两次（一次是 `*this`，一次是 `m_lock`），构造函数出口报
「`m_lock` 仍被持有」，`unlock()` 报「释放未持有的 `m_lock`」。Zircon 的 `lockdep::Guard` 也是这么
分层的：Guard 有注解，它调用的 `LockPolicy::Acquire/Release` 没有。

`unguard()` 标 `TA_RELEASE()` 不是凑数：它的语义正是「我不再负责这把锁，别人会放」，对分析器
来说和释放等价。`Scheduler::reschedule_common` 靠它把 rq 锁交给 `switch_context`。

`ScopedGuard` 是可变参模板，属性参数里不能展开参数包，写不出 `TA_ACQUIRE(locks...)`；只能把
它的实现从分析里摘出去。它的三个调用点（`scheduler.cpp` 的 `tick_thread`/`yield_to_thread`/
`Current::set_state` 附近）以后应换成嵌套 `Guard`，见待修清单。注意 `std::apply` 里的 lambda 对
分析器是独立函数，构造函数上的 `TA_NO_THREAD_SAFETY_ANALYSIS` 管不到它，要单独标。

**改法**
只加注解，不改任何实现。`TA_*` 都展开成 `__attribute__((...))` 的分析属性，对代码生成没有影响。
注解放在声明前缀位置（`TA_ACQUIRE() auto acquire() -> void;`），与本树已有的
`TA_REQUIRES(m_lock) auto base_of(...)` 写法一致；构造函数的 `TA_ACQUIRE(lock)` 要引用形参，
放在形参表之后（`Guard(Lock &lock) TA_ACQUIRE(lock) : ...`），这是 Zircon 的写法，也是唯一能编的
位置。

**参照**
Zircon `lockdep::Guard`（`__TA_SCOPED_CAPABILITY`，构造 `__TA_ACQUIRE(lock)`，`Release()`
`__TA_RELEASE()`）；`fbl::Mutex::Acquire() __TA_ACQUIRE()` / `Release() __TA_RELEASE()` /
`TryAcquire() __TA_TRY_ACQUIRE(true)` / `AssertHeld() __TA_ASSERT()`。

**验证**
- `prebuilt/ninja/ninja -C out/bringup.x86` 全量通过。
- clang-format 已跑；clang-tidy 对 `spin_lock.cpp`/`mutex.cpp` 只剩未改动行上的既有
  `modernize-use-nodiscard`。
- 重编审计用的 15 个 TU：219 → **194** 条。掉的 25 条集中在 `TimerQueue`、`ClockSource`、
  `Dpc::Queue`、`Event`、`GLOBAL_CACHES`——这些地方 `TA_REQUIRES` 和 `Guard` 拼的是同一个
  表达式，分析器现在能对上。剩下的大头是 `TA_REQUIRES(get_lock())` 对不上 `Guard{m_lock}`
  （待修清单第 2 条）。
- QEMU：两个镜像都启动到 tick 设备选定之后、第一个线程入队时在 `Fair::enqueue_thread`
  报 `#DE`。**与本改动无关**：把四个文件还原到改动前重建再启动，`kernel.dev.obi` 在同一阶段
  停在 `SpinLock::assert_held` 断言上（当时调度器那边的权重修复 `bf384ea` 还没提交）；调度器
  目录（`scheduler.cpp`、`scheduler_pi.cpp`、`profiles.hpp`）有另一个会话的在途改动，故障点在
  它们的范围内。纯属性改动改变不了代码生成。

**故意没做**
- 没动 `Mutex::acquire_common()`/`acquire_slowpath()`：分析器没有对 `acquire()` 的函数体报
  「出口期望持锁」，不需要再往下标。
- 没改 `ScopedGuard` 的三个调用点，也没修 `sync.md` 第 1 条（`IrqSave` 不关中断，同一个文件）：
  一次一件事。
- 没把 `-Wno-error=thread-safety` 拿掉：还有 194 条。

---

## 四个 `get_lock()` 类访问器加 `TA_RETURN_CAPABILITY`（2026-09-03）

**文件**
- `kernel/include/ours/task/wait_queue.hpp` — `WaitQueue::get_lock()` → `m_lock`
- `kernel/include/ours/task/thread.hpp` — `Thread::get_pi_lock()` → `m_pi_lock`，
  `get_resource_lock()` → `m_resource_lock`
- `kernel/include/ours/task/sched/scheduler.hpp` — `Scheduler::get_lock()` → `m_runtime.rqlock`
- `kernel/mm/include/ours/mm/pm_node.hpp` — `PmNode::get_lock()` → `m_lock`

**症状**
潜伏。上一条修完后 `WaitQueue::notify_one` 这种写法仍然报警：`Guard guard(m_lock)` 之后调
`dequeue_front_locked()`（声明 `TA_REQUIRES(get_lock())`），分析器说 `get_lock()` 没持有。
`Scheduler::enqueue_thread` 持 `get_lock()` 调 `ISchedule::enqueue_thread`
（`TA_REQUIRES(runtime.rqlock)`）同样报。

**原因**
clang 的分析按**表达式**匹配 capability，不做别名推断。`get_lock()` 是个函数调用表达式，
`m_lock` 是个成员访问表达式，二者对分析器是两个无关的值——即便函数体就一行 `return m_lock;`
它也不去看。`lock_returned` 属性（本树宏名 `TA_RETURN_CAPABILITY`）就是给这种情况用的：它告诉
分析器「这个函数的返回值就是那个 capability」，之后所有 `get_lock()`、`thread->get_pi_lock()`
在注解里都会被替换成 `m_lock`、`thread->m_pi_lock` 再比较。

本树在 `TA_GUARDED`/`TA_REQUIRES` 里几乎全用访问器拼法（成员是私有的，外部只能这么写），
而 `Guard` 的构造实参在类内部又常写成员名，两种拼法并存，没有这个属性就永远对不上。

**改法**
四个访问器各加一个 `TA_RETURN_CAPABILITY(<成员>)`。注解放声明前缀位置，和本树其他 `TA_*`
一致。没有改任何注解的拼法：修过访问器后两种拼法自动等价，不必统一。

`scheduler.hpp` 同时带着另一个会话的在途改动（`STATES` 换 `std::atomic`），只暂存了本条的
hunk，用 `git apply --cached` 从过滤后的 patch 进去，其余两个 hunk 留在工作区。

**参照**
Zircon `Thread::get_lock() __TA_RETURN_CAPABILITY(lock_)`；`fbl::Mutex` 的 `lock()` 也同样标。
clang 文档：`RETURN_CAPABILITY` / `lock_returned`。

**验证**
- 全量构建通过；clang-format 已跑。
- 15 个 TU：194 → **187**。掉的是 `WaitQueue::notify_one/notify_all` 对 `dequeue_front_locked`
  的两处、`MemCache` 的三处、`Dpc` 一处等。更重要的是**剩下的 187 条里的锁名现在全是成员**
  （`m_pi_lock`、`m_runtime.rqlock`），不再有 `get_lock()` 字样——从这里起每一条都是注解或
  代码本身的问题，不再有分析器看不见的成分。
- QEMU：两个镜像都停在 `Fair::enqueue_thread` 的 `#DE`（`+0x2159b` / `+0x1cf23`），与改前
  完全一致；那是调度器那边另一个会话正在处理的权重问题。

**故意没做**
- 没把 `TA_REQUIRES(get_lock())` 改写成 `TA_REQUIRES(m_lock)`：有了 `lock_returned`，两种拼法
  等价，逐条改写只是噪音。
- 没碰 `Scheduler::Current::set_state` 里为满足错误注解而多拿的一次锁（待修清单 3.5）。
