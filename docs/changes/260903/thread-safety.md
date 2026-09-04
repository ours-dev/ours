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

---

## `reschedule_common` 的锁契约写反了，盖住了 `yield_thread` 的自死锁（2026-09-03）

**文件**
- `kernel/include/ours/task/sched/scheduler.hpp` — `reschedule_common` 的 `TA_REQUIRE(get_lock())`
  改为 `TA_REQUIRES(curr->get_pi_lock()) TA_EXCLUDES(get_lock())`
- `kernel/kernel/task/sched/scheduler.cpp` — `Scheduler::Current::yield_thread` 的 `Guard` 收进
  内层块，在调 `reschedule_common` 之前出作用域

**症状**
`Thread::Current::yield()` 第一次被调用时该 CPU 在自己的 runqueue 锁上永久自旋。当前启动流程
还没走到 yield，所以是潜伏的；一旦有第二个线程主动让出就触发。

**原因**
`reschedule_common` 自己的文档注释写得很清楚：「1) 拿 runqueue 锁；……3) 放掉自己拿的锁；
4) 切换」，前置条件是「|curr| 的锁已持有」。但注解写的是 `TA_REQUIRE(get_lock())`——要求调用方
持 runqueue 锁，和第 1 步正好相反。六个调用方（`Current::preempt/reschedule/idle/park_thread/
yield_thread/yield_to_thread`）里五个按文档办事，不持 rq 锁调它；只有 `yield_thread` 按注解办事，
`Guard guard{self.get_lock()}` 还在作用域里就调了进去。`reschedule_common` 里第一行
`Guard guard{get_lock()}` 再拿一次，`SpinLock::do_lock` 是从 `UNLOCKED_STATE` 起的 CAS，不可重入，
于是死锁。

分析器本来能报「acquiring lock that is already held」，但注解说 rq 锁「应该」被持有，它就把
`yield_thread` 的持锁当成满足前置条件，把 `reschedule_common` 内部的再次获取当成……前面第 1、2
条没修时它连 Guard 都看不见，所有告警都是噪音。这一条是那 219 条里唯一一处直接对应真死锁的。

**改法**
注解改成和文档一致：`TA_REQUIRES(curr->get_pi_lock())`（所有调用方都持有，它们自己也是这么
标的）加 `TA_EXCLUDES(get_lock())`（进来时不能持有，因为第 1 步要拿）。`yield_thread` 照旁边
`yield_to_thread` 的写法，把 guard 收进内层块——它对 `request_skip_next_clock_update` 的调用
只需要在 rq 锁下完成，`reschedule_common` 开头的 `accept_skip_next_clock_update` 会在自己的
锁下接住。

**参照**
Zircon `Scheduler::RescheduleCommon()`：`__TA_REQUIRES(thread_lock)`，内部自己拿 queue_lock；
`Scheduler::Yield()` 也是在调 `RescheduleCommon` 前不持 queue_lock。

**验证**
- 全量构建通过；clang-format 已跑；两个文件都带着另一个会话的在途 hunk，只暂存了本条的。
- 15 个 TU：187 → **180**。消失的正是六个调用方的「calling reschedule_common requires holding
  rqlock」和 `scheduler.cpp:508` 的「acquiring rqlock that is already held」。
- QEMU：两个镜像仍停在 `Fair::enqueue_thread` 的 `#DE`，与改前一致；yield 路径在此之前没有
  被走到，**死锁修复未实测**。

**故意没做**
- `reschedule_common` 尾部 `guard.unguard()` + `switch_context()` 仍报一次「releasing rqlock not
  held」：`unguard()` 对分析器是释放，`switch_context` 又标 `TA_RELEASE(get_lock())`，二者重复。
  这是上下文切换交接（待修清单 3.4）的一部分，下一条处理。

---

## `Timer::trylock_or_cancel` 标 `TA_TRY_ACQUIRE(false, lock)`（2026-09-03）

**文件**
- `kernel/include/ours/time/timer.hpp` — 声明加 `TA_TRY_ACQUIRE(false, lock)`

**症状**
潜伏。`WaitQueue::handle_timeout`（`wait_queue.cpp:271-282`）
`if (timer->trylock_or_cancel(pi_lock)) return;` 之后的 `pi_lock.release()` 被报「releasing lock
that was not held」，`timer.cpp:102` 被报「lock is not held on every path」。

**原因**
`try_acquire_capability` 的第一个参数是「表示拿到了锁的返回值」，默认习惯是 `true`。这个函数
是 Zircon `Timer::TrylockOrCancel` 的形状：返回 **false** 表示锁已到手、**true** 表示 timer 正在
被 cancel、放弃。分析器不知道这个反转，把 `if (...) return;` 当成成功分支，于是 fall-through 路径
上的 `release()` 变成释放没拿到的锁。它自己的函数体也一样：`return false` 那条路径持锁、
`return true` 不持，不标就是「不是每条路径都持锁」。

**改法**
一行注解 `TA_TRY_ACQUIRE(false, lock)`，成功值写 `false`。注释里把「为什么是 false」写明，因为它
和所有 `try_*` 的直觉相反，下一个读的人一定会想把它改成 `true`。

**参照**
Zircon `Timer::TrylockOrCancel(MonitoredSpinLock *lock) TA_TRY_ACQ(false, lock)`。

**验证**
- 全量构建通过（中途一次失败是另一个会话新加的 `rr.cpp` 访问私有成员，他们随即修了）；
  clang-format 已跑；clang-tidy 对 `timer.cpp` 在改动行无新告警。
- 15 个 TU：180 → **178**，消失的正是上面两条。
- QEMU：这次两个镜像停的位置都变了——`kernel.dev.dbg_1.obi` 走到 562 行停在 canary magic 断言，
  `kernel.dev.obi` 停在 `SpinLock::assert_held` 断言——因为调度器那边同一时段合入了新的
  discipline（`rr.cpp`）。两处都不在本改动可能影响的范围内：注解不改代码生成。

**故意没做**
无。

---

## 删掉 `TA_GUARD`/`TA_REQUIRE`/`TA_CAP` 三个兼容拼法（2026-09-03）

**文件**
- `kernel/mm/include/ours/mm/object_cache.hpp` — 两处 `TA_GUARD` → `TA_GUARDED`
- `kernel/mm/include/ours/mm/pm_node.hpp` — 三处 `TA_GUARD` → `TA_GUARDED`（加一处注释）
- `system/public/ours/compiler.hpp` — 删掉三个兼容宏
- `.agents/skills/language/modern-cpp-development/SKILL.md` — 「两种拼法都能编」改为「短拼法已删」

**症状**
无。同一个属性两种名字并存，读的人要多记一层映射，新代码会照着最近看到的那个抄。

**原因**
`compiler.hpp` 里注明这三个是「pre-refactor kernel headers 的兼容拼法」，即计划中就是要退役的。
`TA_REQUIRE` 唯一的使用点已在 3.1 里随 `reschedule_common` 的注解一起改掉，`TA_CAP` 无人用，
剩五处 `TA_GUARD`。留着兼容宏没有任何收益，删掉之后再写短拼法直接编不过，比靠文档约束可靠。

**改法**
纯替换加删宏。`.agents` 下的风格文档同步改，否则它还在说「both currently work」。

**参照**
Zircon 只有一套 `__TA_*`（`zircon/compiler.h`），没有别名。

**验证**
- 全量构建通过（删宏触发全树重编）；clang-format 已跑。
- 15 个 TU 告警 178 不变——纯改名，预期如此。
- QEMU：两个镜像停的位置与上一条相同。

**故意没做**
`mutex.hpp` 的 `CirticalMutex` 拼错单独一条改。

---

## `CirticalMutex` → `CriticalMutex`（2026-09-03）

**文件**
- `kernel/include/ours/sync/mutex.hpp` — 类名、`Self` 别名、`TA_CAPABILITY` 里的名字

**症状**
无。拼错的名字，无调用方。

**原因**
名字错了就是缺陷，不是外观问题：它出现在 `TA_CAPABILITY("...")` 里，分析器的每条相关告警都会
把这个错拼法打出来。趁没有调用方改掉，成本最低。

**改法**
三处替换。capability 名从 `"Cirtical-Mutex"` 改为 `"CriticalMutex"`，与同文件 `"Mutex"`、
`"SpinLock"` 的风格一致（不带连字符）。

**参照**
无。

**验证**
全量构建通过；两个镜像停的位置与上一条相同。

**故意没做**
没有给这个空壳类补实现——它是 timeslice extension 的占位，另有安排。

---

## `Scheduler::unpark_thread` 的契约：调用方持 PI 锁并自己放，修掉 `suspend_or_kill` 的两处锁泄漏（2026-09-03）

**文件**
- `kernel/include/ours/task/sched/scheduler.hpp` — `unpark_thread(Thread *)` 的
  `TA_ACQUIRE(thread->get_pi_lock())` 改为 `TA_REQUIRES(thread->get_pi_lock())`
- `kernel/kernel/task/thread.cpp` — `suspend_or_kill()` 的 Suspend+Kill 分支和 Sleeping 分支删掉
  调 unpark 前的 `thread_guard.unguard()`

**症状**
潜伏，当前启动流程还没 kill 过挂起/睡眠的线程。一旦发生：该线程的 PI 锁被本 CPU 永久持有，
之后任何一次对它的 `acquire()`（哪怕来自同一 CPU）会撞 `DEBUG_ASSERT(!is_held())`，非调试
镜像则直接自旋到死。

**原因**
`unpark_thread(Thread *)` 的实现（`scheduler.cpp:187-204`）对 PI 锁一个字都没碰，只是选 CPU、
调 `do_unpark_thread`。但声明写的是 `TA_ACQUIRE(thread->get_pi_lock())`——「调完这个函数你就
持有了 PI 锁」，哪个方向都不对。六个调用方对它的理解分成两派：

- `Thread::resume()`、`WaiterState::unsleep()`、`WaitQueue::handle_timeout()`：调用前持锁，调用后
  自己放（handle_timeout 是手动 `pi_lock.release()`）。这派是对的。
- `Thread::suspend_or_kill()` 的两个分支：先 `thread_guard.unguard()` 告诉 Guard「不用你放了」，
  再调 unpark——以为 unpark 会替它们放。没人放，锁就漏了。

原注释「Invoke it before to handle the following cases since in the code bottom thread switching
may happens」透露了误解的来源：作者以为 unpark 会切换上下文并在切换路径上放锁。那是
`reschedule_common` 的行为，不是 unpark 的——unpark 只入队，抢占请求走 `unpark_preempt`。

**改法**
注解改成 `TA_REQUIRES(thread->get_pi_lock())`：「进来时持有、出去时仍持有」，与实现和多数派
调用方一致。删掉两处 `unguard()`，Guard 在作用域尾正常放锁。`UnparkList` 重载自己对每个线程
`Guard` 一遍，不需要注解。

**参照**
Zircon `Scheduler::Unblock(Thread *) TA_REQ(thread_lock)`；`Thread::Resume()` 和 `Thread::Kill()`
都在持 `thread_lock` 的 Guard 作用域里调它，作用域结束放锁。

**验证**
- 全量构建通过；clang-format 已跑；clang-tidy 对 `thread.cpp` 改动行无新告警；`scheduler.hpp`
  只暂存了本条的 hunk。
- 15 个 TU：178 → **174**。消失的是 `thread.cpp:201/203` 的「acquiring m_pi_lock already held」、
  `thread.cpp:272` 的「not held on every path」（就是那两处 unguard）、`scheduler.cpp:205` 的
  「expecting thread->m_pi_lock held at end」。`wait_queue.cpp:19` 的「still held at end」变成
  `:18` 的「calling unpark_thread requires holding」——`WaiterState::unsleep` 自己还没标
  `TA_REQUIRES`，是待修 4.4。
- QEMU：两个镜像停的位置与上一条相同。kill 路径未被走到，**泄漏修复未实测**。

**故意没做**
- `WaiterState::sleep/unsleep`、`park_thread_common` 的 `TA_REQUIRES` 传播（4.4）下一条。

---

## `MemCache::SLAB_NODES` 去掉 `TA_GUARDED(CACHE_LOCK)`（2026-09-03）

**文件**
- `kernel/mm/include/ours/mm/object_cache.hpp` — 删注解，加注释说明为什么不需要锁

**症状**
潜伏。`object_cache.cpp:374`（slab 补充路径读 `SLAB_NODES`）和 `:494`（`init()` 写）报「requires
holding Mutex CACHE_LOCK」。

**原因**
注解说它归 `CACHE_LOCK`，但没有一个调用方在拿它，也不该拿：`CACHE_LOCK` 是睡眠 `Mutex`，
`SLAB_NODES` 的读者是分配热路径。而它只在 `MemCache::init()` 里被写一次，那时候第一个 cache 还
不存在，之后再没有写者——没有写者就没有东西要互斥。这条注解描述的是一个不存在的保护关系。

**改法**
删掉 `TA_GUARDED`，注释写清「init 写一次、之后只读、热路径读」这三点，以及为什么不能是
`CACHE_LOCK`。`GLOBAL_CACHES` 的注解保留：它在 `init()`（每个 cache 的）里被 `push_back`，
确实在 `CACHE_LOCK` 下。

**参照**
Linux `slab_nodes`：`slab_node_online()/offline()` 在 `slab_mutex` 下写（热插拔路径），分配器
无锁读。本树还没有 node 热插拔，写者只有 bring-up。

**验证**
- 全量构建通过；clang-format 已跑。
- 15 个 TU：174 → **172**。
- QEMU：`kernel.dev.dbg_1.obi` 这次停在 `curr == self.m_runtime.curr_thread` 断言（调度器那边又
  合入了改动），`kernel.dev.obi` 仍是 `assert_held` 断言。本改动只删了一个属性。

**故意没做**
没有把 `SLAB_NODES` 做成 `const`：node 热插拔来了它就要可写，届时应照 Linux 在 `CACHE_LOCK`
下写、无锁读，再把 `TA_GUARDED` 加回来就是错的——那时需要的是 RCU 式的「写者互斥、读者无锁」，
分析器表达不了。

---

## `PmNode` 的三个 CpuMask：两个不再声称有锁，一个改为持锁拷出（2026-09-03）

**文件**
- `kernel/mm/include/ours/mm/pm_node.hpp` — `m_hard_affinity`/`m_soft_affinity` 去掉
  `TA_GUARDED(get_lock())`；`work_cpus()` 改为持锁返回副本；加 `#include <ours/lockdep.hpp>`
- `kernel/mm/include/ours/mm/pmm.hpp`、`kernel/mm/pmm.cpp` — `node_work_cpus()` 随之改为按值返回，
  节点不存在时返回空 mask

**症状**
潜伏。`hard_affinity()`/`soft_affinity()`/`work_cpus()` 三个访问器把受保护成员按 `const &` 返回，
静态 `hard_affinity()` 把它按引用传给 `CpuLocal::access`，`init()` 裸写 `m_hard_affinity`——
每个 TU 各报一遍，共 45 条，占当时剩余告警的四分之一。

**原因**
三个 mask 的生命周期不一样，原来却贴了同一个注解。

- `m_hard_affinity`/`m_soft_affinity`：`PmNode::init()` 在把节点标为 Online 之前写一次，之后
  只读。而且 `cpu_local.cpp:314` 把 `node_hard_affinity()` 返回的**指针**存起来长期用——一个被
  锁保护的对象是不可能把裸引用交出去的，这种用法本身就说明它不在锁下。注解描述的保护关系不
  存在，读者一个都没拿过锁，也拿不了。
- `m_work_cpus`：`update_cpu_to_node()` 在 CPU 换节点时在锁下改它（`pm_node.cpp:762-813`，
  那三处 `Guard` 是对的）。它确实需要锁。但 `work_cpus()` 按引用返回，等于把锁下的值交给一个
  不持锁的调用方——下一次换节点就在调用方脚下改它。

**改法**
前两个删注解，注释写清「init 写一次、Online 后只读、有人存指针」。`m_work_cpus` 保留注解，
`work_cpus()` 改成 `Guard guard{get_lock()}; return m_work_cpus;`——按值拷出是不持锁情况下唯一
能交出去的东西；`CpuMask` 是 `MAX_CPU` 位的位图，拷贝不贵。`node_work_cpus()` 目前没有调用方，
一起改成按值，「不存在的节点」和「没有 CPU 的节点」都返回空 mask，注释里说明了这个合并。

否掉的方案：给三个访问器都加 `TA_REQUIRES(get_lock())`。对前两个是把一个假保护关系推给所有
调用方去演；对 `work_cpus()` 则逼 `pmm.cpp` 那层指针 API 在锁下返回指针，同样是把引用带出锁。

**参照**
Linux `node_to_cpumask_map[]`：bring-up 写，之后无锁读，返回 `const struct cpumask *`；
`cpumask_of_node()` 没有锁。`m_work_cpus` 对应 Linux 里没有直接物——Linux 用 `cpu_to_node_map`
单向映射，不维护反向集合。

**验证**
- 全量构建通过；clang-format 已跑；clang-tidy 对 `pmm.cpp` 改动行无新告警。
- 15 个 TU：172 → **127**。`pm_node.hpp`/`pm_node.cpp` 相关告警全部清零。
- QEMU：两个镜像停的位置与上一条相同。
- `pmm.cpp` 带着另一个会话的一个 hunk（`alloc_frames`），只暂存了本条的。

**故意没做**
没有把 `m_hard_affinity` 的写入挪到锁下——它在 Online 之前，没有并发读者。

---

## 三个「函数体内拿了又放」的函数错标了 `TA_ACQUIRE`（2026-09-03）

**文件**
- `kernel/include/ours/task/sched/scheduler.hpp` — `do_unpark_thread`：`TA_ACQUIRE(get_lock())` →
  `TA_REQUIRES(thread->get_pi_lock()) TA_EXCLUDES(get_lock())`；`process_migrating_threads`：
  `TA_ACQUIRE(m_runtime.migrating_threads_lock)` → `TA_EXCLUDES(...)`；`unpark_common`（只有声明）
  去掉 `TA_ACQUIRE(get_lock())`。顺带改掉 `do_unpark_thread` 上「会放掉 |thread| 的锁」的错注释。

**症状**
潜伏。`scheduler.cpp` 的 `do_unpark_thread` 和 `process_migrating_threads` 出口各报一条
「expecting lock to be held at the end of function」，`unpark_thread(UnparkList)` 的循环报
「still held at end」和「expecting held at start of each loop」。

**原因**
`TA_ACQUIRE(L)` 的意思是「返回时调用方持有 L」——这是 `lock()` 一类函数的契约，函数把锁拿了
**不放**，留给调用方。这三个函数的函数体都是 `Guard guard{...}` 拿了就放，出口不持锁。注解和
实现方向相反，所以分析器在每个出口都问「你答应留给我的锁在哪」。

`do_unpark_thread` 的注释还写着「This method will drop the |thread|'s lock」，和 3.2 里
`unpark_thread` 的错误是同源的：作者以为 unpark 会顺着切换路径把 PI 锁放掉。它不切换，也不放。

**改法**
「进来不能持有、出去也不持有、中间自己拿放」的正确拼法是 `TA_EXCLUDES(L)`。
`do_unpark_thread` 另加 `TA_REQUIRES(thread->get_pi_lock())`，因为它改线程状态、入队，调用方
（`unpark_thread` 两个重载）都持着。`unpark_common` 没有函数体，锁契约无从谈起，先不标。

**参照**
Zircon `Scheduler::Unblock(Thread *) TA_REQ(thread_lock)`——内部 `Guard<MonitoredSpinLock>
queue_guard{&queue_lock_}` 拿放 queue_lock，对外不暴露；`TA_EXCL` 在 Zircon 里用于同类场合
（如 `Thread::Kill() TA_EXCL(thread_lock)`）。

**验证**
- 全量构建通过；clang-format 已跑；`scheduler.hpp` 只暂存了本条的三个 hunk。
- 15 个 TU：127 → **123**。
- QEMU：`kernel.dev.dbg_1.obi` 这次跑到 819 行、停在一个 `format_error`（调度器那边的新改动），
  `kernel.dev.obi` 仍是 `assert_held` 断言。本改动只改注解和注释。

**故意没做**
无。

---

## 上下文切换的锁交接：三个函数退出分析，而不是错标 `TA_RELEASE`（2026-09-03）

**文件**
- `kernel/include/ours/task/sched/scheduler.hpp` — `switch_context`、`finish_switch_context` 的
  `TA_RELEASE(get_lock())` 换成 `TA_NO_THREAD_SAFETY_ANALYSIS`；`trampoline_handoff` 加
  `TA_NO_THREAD_SAFETY_ANALYSIS`；协议写在 `switch_context` 的注释里

**症状**
潜伏。六条告警：`switch_context` 里放 rq 锁被报「not held」（因为 `reschedule_common` 的
`unguard()` 已经算释放过一次），`finish_switch_context` 放 prev 的 PI 锁被报「not held」、出口
被报「rq 锁 still held」，`trampoline_handoff` 两处 release 被报「not held」，
`reschedule_common` 调 `switch_context` 被报「releasing not held」。

**原因**
两件事分析器都表达不了：

1. **Guard 交接**。`reschedule_common` 用 `Guard` 拿 rq 锁，切换前 `guard.unguard()` 告诉 Guard
   「别放了」，由 `switch_context` 在记录 prev/curr 之后、换栈之前手动放。对分析器 `unguard()`
   就是一次释放（3.1 之前的第 1 条改动里定的），`switch_context` 再标 `TA_RELEASE` 就是第二次。
   把 `unguard()` 改成不算释放也不行：那 Guard 析构又会被算成释放。
2. **跨激活释放**。`finish_switch_context` 放的是 `m_runtime.prev_thread` 的 PI 锁——由**上一个**
   线程在**它的**栈上拿的，在**下一个**线程的栈上放。分析器的模型是「一个函数激活内拿放配平」，
   对这种「A 拿、B 放」它连表达都做不到，标什么都是错的。`trampoline_handoff` 是新线程第一次
   跑到的那一端，同理。

之前的 `TA_RELEASE(get_lock())` 对 `switch_context` 半对（它确实放 rq 锁），对
`finish_switch_context` 全错（它放的是 PI 锁不是 rq 锁）。

**改法**
三个函数 `TA_NO_THREAD_SAFETY_ANALYSIS`，交接协议写在 `switch_context` 的声明注释里：谁拿、
在哪放、为什么分析器跟不上。可检查的那半——「调用方持 curr 的 PI 锁」——已经在
`reschedule_common` 的 `TA_REQUIRES` 上（3.1）。

**参照**
Zircon `Scheduler::FinalContextSwitch()` 和 `Thread::Current::Reschedule()` 围着 `thread_lock`
交接的部分同样是 `__TA_NO_THREAD_SAFETY_ANALYSIS`；Linux `finish_lock_switch()` 放的是
`rq->lock`，同样靠注释而不是注解（Linux 用 sparse 的 `__releases`，也只是文档）。

**验证**
- 全量构建通过；clang-format 已跑；`scheduler.hpp` 只暂存了本条的两个 hunk。
- 15 个 TU：123 → **117**。scheduler 目录还剩 8 条：`set_state`（3.5）4 条、migrator（4.5）2 条、
  `activate/deactivate_thread` 缺 `TA_REQUIRES` 2 条、`finish_transition`/`reschedule_mask` 各 1。
- QEMU：两个镜像停的位置与上一条相同。

**故意没做**
没有把 `guard.unguard()` + 手动 release 改成 `guard.unlock()`：release 必须发生在
`switch_context` 记完 prev/curr、切完 aspace 之后，挪出去就得把这些写操作也挪出去。

---

## `Scheduler::set_state` 改为无锁 CAS，去掉保护不了它的 `TA_REQUIRES(get_lock())`（2026-09-03）

**文件**
- `kernel/kernel/task/sched/scheduler.cpp` — `set_state` 由 load/set/store 改为
  `compare_exchange_weak` 循环
- `kernel/include/ours/task/sched/scheduler.hpp` — 删 `TA_REQUIRES(get_lock())`；
  `Current::set_state` 删掉只为满足它而拿的 `Guard`

**症状**
潜伏，SMP 才触发：两个 CPU 同时进/出 idle，各自 load 旧 mask、改自己的位、store 回去，后写的
把先写的那一位冲掉，`STATES[Idling]` 少一个 CPU。后果是 `peek_active_mask()`/IPI 目标选择看到
错误的 CPU 集合。单核启动看不见。

分析器侧：`scheduler_inline.hpp:14/28`（`put_prev_thread`/`set_next_thread`）各报 2 条
「calling set_state requires holding rqlock」。

**原因**
`STATES` 是全局数组，每个元素一个 `std::atomic<CpuMask>`，**所有 CPU** 改同一个字里各自的位。
本 CPU 的 runqueue 锁只互斥本 CPU 上的调度器操作，对另一个 CPU 的 RMW 毫无约束。所以
`TA_REQUIRES(get_lock())` 承诺的互斥根本不存在——注解在这里制造了「有锁保护」的错觉，还让
`Current::set_state` 专门拿一次 rq 锁去满足它，白白多一次自旋锁。正确的原语是原子 RMW。

**改法**
CAS 循环。`std::atomic<BitSet>` 没有 `fetch_or`（非整型），所以是
`load → 拷贝改位 → compare_exchange_weak` 直到成功；`memory_order_relaxed` 足够，这个 mask 只是
IPI 目标的提示，不发布其他数据。注解删掉，`Current::set_state` 的 Guard 删掉。

否掉的方案：给 `CpuMask` 加原子 `fetch_or`。`BitSet` 是多字位图，原子只能按字做，等于重写一个
`AtomicBitSet`；`STATES` 是它唯一的原子用途，不值。

**参照**
Zircon `mp.idle_cpus.fetch_or(cpu_num_to_mask(cpu))` / `fetch_and(~...)`
（`kernel/kernel/mp.cc`，`mp_set_curr_cpu_active` 等）。

**验证**
- 全量构建通过；clang-format 已跑；clang-tidy 对改动行无新告警；两个文件都只暂存了本条 hunk。
- 15 个 TU：117 → **113**。
- QEMU：两个镜像停的位置与上一条相同（单核，竞争路径不会触发，**竞态修复未实测**）。

**故意没做**
无。

---

## `activate/deactivate_thread` 补 `TA_REQUIRES(get_lock())`；`Migrator::move_queued_thread` 的换锁退出分析（2026-09-03）

**文件**
- `kernel/include/ours/task/sched/scheduler.hpp` — `activate_thread`/`deactivate_thread` 加
  `TA_REQUIRES(get_lock())`
- `kernel/kernel/task/sched/scheduler_migrator.cpp` — `move_queued_thread` 加
  `TA_REQUIRES(self->get_lock())` + `TA_NO_THREAD_SAFETY_ANALYSIS`，注释写明它「进来持 A 的锁、
  出去持 B 的锁」

**症状**
潜伏。`scheduler.cpp` 里 `activate_thread`/`deactivate_thread` 调 `enqueue_thread`/`dequeue_thread`
各报一条「requires holding rqlock」；`scheduler_migrator.cpp:16/24` 报「releasing not held」和
「still held at end」。

**原因**
`activate/deactivate_thread` 是 `enqueue/dequeue_thread` 的薄封装，后者标了 `TA_REQUIRES(get_lock())`，
前者没标——注解链在这一层断了，于是分析器在封装内部报、对封装的调用方却什么都不查。唯一的
调用方 `do_unpark_thread` 和 `Migrator` 都持着锁，是纯漏标。

`Migrator::move_queued_thread` 是另一回事：它持着源 CPU 的 rq 锁进来，中途 `release()` 掉、
`acquire()` 目标 CPU 的 rq 锁，持着**另一把**锁返回。分析器能说「需要 L」「获得 L」，说不出
「把 L 换成 M」。它用裸 `acquire()/release()` 而不是 Guard 也是这个原因——Guard 同样表达不了换锁。

**改法**
前者补 `TA_REQUIRES(get_lock())`。后者标可检查的那半（进入时持 `self->get_lock()`），函数体
`TA_NO_THREAD_SAFETY_ANALYSIS`，注释说清换锁协议和为什么用裸锁。

**参照**
Linux `move_queued_task()`：`lockdep_assert_rq_held(rq)` 进入，`rq_unlock(rq)` →
`rq_lock(new_rq)`，返回 `new_rq`——同样的换锁形状，同样只靠断言检查进入条件。

**验证**
- 全量构建通过；clang-format 已跑；`scheduler.hpp` 只暂存了本条的 hunk。
- 15 个 TU：113 → **108**。调度器目录只剩 `finish_transition`（`scheduler.cpp:449`，从
  `process_migrating_threads` 的 lambda 里不持 PI 锁调它）一条，它是个只有断言的桩，归调度器
  那边的迁移工作，记在待修清单。
- QEMU：`kernel.dev.dbg_1.obi` 仍停在 `format_error`；`kernel.dev.obi` 这次在「keeps tick device
  HPET over LVT」之后静默挂住（46 行，没有断言）——中间合入了调度器那边的 `456e176`，本改动
  只加注解和注释。

**故意没做**
`finish_transition` 的锁契约：函数体是桩，等它有实现再定。

---

## 两条唤醒路径补上被唤醒线程的 PI 锁（2026-09-03）

**文件**
- `kernel/kernel/task/wait_queue.cpp` — `WaitQueue::wake_dequeued_thread()` 用
  `Guard<SpinLock, IrqSaveAndPreemptDisable>` 拿 `thread->get_pi_lock()`，包住 PI 传播和
  `unsleep()`；补 `#include <ours/task/auto_preempt_disabler.hpp>`
- `kernel/include/ours/task/wait_queue.hpp` — `wake_dequeued_thread` 的注释写明它自己拿 PI 锁
- `kernel/kernel/task/thread.cpp` — `Thread::Current::sleep_etc()` 的 timer 回调改成
  `trylock_or_cancel(pi_lock)` → `unsleep` → `release()`，与 `WaitQueue::handle_timeout` 同形

**症状**
潜伏，单核启动流程走不到。`notify_one/notify_all` 唤醒一个 waiter、或 `sleep_for` 超时唤醒时，
`Scheduler::unpark_thread` → `do_unpark_thread` 改线程状态、入队，全程不持该线程的 PI 锁；与
同一时刻持 PI 锁读 `state()` 的 `suspend_or_kill()`、`resume()` 竞争。

**原因**
3.2 定下的契约是 `unpark_thread` **要求**调用方持 PI 锁。六个调用链里四个满足：`resume()`、
`suspend_or_kill()`（Guard）、`WaitQueue::handle_timeout()`（`trylock_or_cancel`）、
`Event`/`Mutex` 走 `UnparkList` 重载（自己 Guard）。剩下两条不满足：

- `wake_dequeued_thread()`：`notify_one/notify_all` 在放掉队列锁后调它，它直接 `unsleep`。
- `sleep_etc()` 的 timer 回调：直接 `unsleep`，而同一文件里的 `handle_timeout` 早就用
  `trylock_or_cancel` 先拿锁了——两个 timeout 路径一个对一个错。

分析器没报这两处，是因为 `WaiterState::unsleep` 自己标不了 `TA_REQUIRES(thread->get_pi_lock())`
（`Thread` 在 `wait_queue.hpp` 里不完整，见待修 4.4），注解链在它这里断了；按契约人工核对
才找到（待修 4.7）。

**改法**
- `wake_dequeued_thread`：`Guard<SpinLock, IrqSaveAndPreemptDisable>`。锁序「PI 在外、队列在内」
  没有被反转，因为调用方已经放了队列锁——这正是它注释里「Must be called with the queue lock
  dropped」的原因，现在注释把「它自己拿 PI 锁」补上了。关抢占和 `Thread::resume()` 一致：被唤醒
  的线程不能在我们还持它的锁时抢占我们。PI 传播也放进锁里：`begin_propagate` 读
  `thread.scheduling_context()`，那是 PI 锁保护的。
- timer 回调：照 `handle_timeout`。`trylock_or_cancel` 返回 **false** 表示拿到（3.6 的注解），
  `cancel()` 那一侧在 `sleep_etc` 的 Guard 作用域之外，所以不会持着 PI 锁等一个在等 PI 锁的
  handler。

**参照**
Zircon `WaitQueue::WakeOne()`/`WakeAll()`：整个唤醒在 `thread_lock` 下；`Thread::SleepEtc()` 的
`SleepTimerCallback` 是 `TrylockOrCancel(thread_lock)` → `Unblock` → `Release`。Linux
`try_to_wake_up()` 第一件事是 `raw_spin_lock_irqsave(&p->pi_lock)`。

**验证**
- 全量构建通过；clang-format 已跑；clang-tidy 对两个文件改动行无新告警。
- 15 个 TU 告警 108 不变——预期如此，这两处本来就没被报（见原因）。
- QEMU：**未实测**。两个镜像当前都停在调度器/prof 那边正在处理的位置（`kernel.dev.dbg_1.obi`
  在 `Free frame(F000, 0)` 之后静默自旋，`kernel.dev.obi` 在 tick 设备选定后静默挂住），
  单核启动也走不到 notify/timeout 唤醒。

**故意没做**
`WaiterState::sleep/unsleep` 的 `TA_REQUIRES`（4.4）：头文件包含方向的问题，另立一条。
