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
