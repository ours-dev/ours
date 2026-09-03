# kernel/task（线程）— 改动记录

格式见 `docs/changes/TEMPLATE.md`。调度器那一层的记录在 `scheduler.md`，这里只放 `Thread` 本身。

---

## 1. `Thread::spawn` 模板的实参顺序反了，一直在调用自己（2026-09-03）

**文件**
- `kernel/include/ours/task/thread.hpp` — 两个 `spawn` 模板体里回传给非模板重载的实参顺序。

**症状**

`task::init()` 跑完之后，`start_kernel()` 一 spawn "laborer" 就停：

```
[... cpu0 error] operator new
```

没有别的信息。内核的全局 `operator new` 是 `kernel/lib/libc/cpp_dependencies.cpp` 里的一个
panic 桩（`ours::panic(__func__)`），所以任何走到它的分配都长这样。

**原因**

非模板重载的声明是「名字、例程、优先级」：

```cpp
static auto spawn(char const *, StartRoutine, usize priority) -> Self *;
static auto spawn(char const *, StartRoutine, sched::BaseProfile const &) -> Self *;
```

模板重载自己的参数表却是「名字、优先级、例程、参数包」，而模板体把 bind 好的
`entry_point` **按自己的顺序**回传：

```cpp
StartRoutine entry_point = ustl::function::bind(entry, ustl::forward<Args>(args)...);
return Self::spawn(name, priority, entry_point);   // (name, usize, std::function)
```

`(char const *, usize, std::function<int()>)` 对非模板重载的 `(char const *, StartRoutine, usize)`
不可行——第二个位置要 `StartRoutine`，给的是 `usize`。唯一可行的候选是模板自己，`F` 推导成
`StartRoutine`。于是它再 bind 一次：`std::function` 套 `std::bind` 套 `std::function`，
然后再调自己，无限递归。

它没真的递归下去，是因为**第二层就分配了**。用宿主 libc++ 复现（`scratchpad/overload.cpp`）：

| 对象 | 大小 |
|---|---|
| `std::function<int()>` 的 SBO 容量 | 32 |
| `std::bind(函数指针)` | 16 —— 塞得下 |
| `std::bind(std::function<int()>)` | 40 —— 塞不下 |

第一层 bind 一个裸函数指针，16 字节，`std::function` 内联持有；第二层 bind 的是上一层的
`std::function`，40 字节，超出小缓冲，`std::function` 去堆上要空间，撞上 panic 桩。
lldb 的栈正好是这个形状：`spawn<int(*)()>` 内联调用 `spawn<std::function<int()>>`，
后者在 `function.h:464` 调 `operator new(40)`。

同一个复现在两种写法下跑：错的写法模板深度打到上限、从未到达非模板重载；对的写法深度 1、
到达。

这类错误的根子是两个重载家族的参数顺序不一致：非模板是「例程在前」，模板是「优先级在前」。
只要顺序一致，回传就不可能选错。顺序不一致本身没改（它牵扯所有调用点），这里只修回传。

**改法**

两个模板体都改成按非模板重载声明的顺序回传：

```cpp
return Self::spawn(name, entry_point, priority);
return Self::spawn(name, entry_point, profile);
```

`(char const *, StartRoutine, usize)` 对非模板是精确匹配；两个模板都不可行（第二个位置
既不是 `usize` 也不是 `BaseProfile`），所以不再有歧义。

**参照**

无，本项目自有设计。这是一个纯 C++ 重载决议问题，不是操作系统设计问题。

**验证**

- 构建通过，`clang-format` 无改动，`clang-tidy` 无新告警。
- 宿主 libc++ 下的独立复现（见上表）证实了机制：错误写法递归、正确写法一次到达。
- **QEMU 未实测到这一步**：树里 mm 的在途改动（`VmArea::reserve()` → `VmMapping::protect()`
  在 `init_vmm()` 里撞 canary，见 `docs/repairs/260903/mm.md` 或对应会话）让启动停在
  `init_vmm()`，走不到 `task::init()`，更走不到 spawn "laborer"。这条改动的证据是上面
  的复现和 lldb 栈，不是启动日志。等 mm 修好要回来补一次实测。

**故意没做**

- 没有统一两个重载家族的参数顺序。那是接口改动，动所有 `Thread::spawn` 调用点，超出本条。
- 没有处理「`Thread::spawn` 绑定的参数一多就会超出 SBO 撞 panic 桩」这个更深的问题。
  `StartRoutine` 是 `std::function`，而这个内核没有可用的全局 `operator new`，所以任何
  绑定超过 ~2 个指针大小参数的 spawn 都会在这里死。已记进 `docs/repairs/260903/task.md`。

---

## 2. `m_owned_wait_queues` 统一到 pi 锁，并修掉 `disown_all_queues` 的错位 erase（2026-09-03）

**文件**
- `kernel/include/ours/task/thread.hpp` — `m_owned_wait_queues` 补 `TA_GUARDED(get_pi_lock())`，
  并写明为什么是 pi 锁而不是 resource 锁。
- `kernel/kernel/task/owned_wait_queue.cpp` — `set_owner()` / `reset_owner()` 的断言统一到
  pi 锁；`disown_all_queues()` 去掉 swap，改为全程持 pi 锁排空。

**症状**

潜伏，当前触发不到。`assign_owner()`、`park_and_assign_owner()`、`disown_all_queues()`
在全树都还没有调用者（`grep` 过），所以 `set_owner`/`reset_owner` 这一条边目前不会被走到。
一旦 Mutex 的 notify 路径写完（`lock_for_notify_operation` 现在是返回 `NONE` 的桩），
它们立刻变成热路径。

会触发两个独立的问题：

1. 同一条链表被两把锁分别改。`set_owner()` 在 pi 锁下 `push_back`，`reset_owner()` 断言的却是
   resource 锁，`disown_all_queues()` 也用 resource 锁去 swap。两条路径可以真并发，
   侵入式链表的指针会被撕开。
2. `disown_all_queues()` 的 erase 打在了错的链表上。它先把链表整个 swap 到局部的 `owqs`，
   再 `pop_front()` 把节点摘下来，然后调 `reset_owner()`——而 `reset_owner()` 做的是
   `m_owner->m_owned_wait_queues.erase(iterator_to(*this))`。此时 `*this` 既不在
   `m_owner` 的链表里（已被 swap 走），也不在 `owqs` 里（已被 pop）。对一个不属于该链表的
   节点调 `erase`，侵入式容器不会报错，它只会照着节点里的 prev/next 指针接线并把 size 减一，
   于是被摘空的那条链表 size 变成 -1（`usize` 下即回绕成巨大值）。

**原因**

第一个问题的根子是 `Thread` 的两把锁职责没划清。这两把锁是照 Linux 的 `task_struct` 做的：
`p->pi_lock` 管线程在调度/阻塞图里的位置——PI 的边（`pi_waiters`、`pi_blocked_on`）、
`p->__state` 的迁移、唤醒的串行化；`p->alloc_lock`（`task_lock()` 拿的那把）管这个任务
**持有哪些对象**——`->mm`、`->fs`、`->files`、`->comm`，它在调度器路径上从不出现。

`m_owned_wait_queues` 是「本线程作为 owner 向外发出的 PI 边」，按这条线毫无疑问归 pi 锁。
它归 resource 锁不只是「标错了」，而是会让后面的 ChainLock 无法成立：PI 链的游走形态是
thread -> 它拥有的队列 -> 队列的 owner -> ……，每一跳到达一个线程时手上只有 pi 锁
（`assign_owner()` 已经在 `assert_held()` 这个前提了）。如果边集合挂在另一把锁上，每跳就要
同时持两个不同锁类的锁，而「按 token 比大小、输了就全放退避」的协议只能对**单一锁类**
建立全序，两个锁类混在一条链上就重新有环了。

第二个问题的根子是把「缩短临界区」和「转移所有权」混为一谈。swap 出来的意图显然是想早点放锁，
但 `reset_owner()` 的语义里包含了「从 owner 的链表上摘下自己」，swap 恰好抽掉了它赖以工作的
前提。侵入式容器在这里没有任何保护——节点自己带着 hook，`erase` 不会去问「你真在我这条链上吗」。

**改法**

- `m_owned_wait_queues` 加 `TA_GUARDED(get_pi_lock())`。
- `set_owner()` / `reset_owner()` 都改成 `assert_held()` pi 锁。前置条件写不成 `TA_REQUIRES`：
  锁是通过一个分析器跟不动的指针取到的，`reset_owner()` 更是通过成员 `m_owner`，调用点上
  根本没有可命名的表达式。所以标 `TA_NO_THREAD_SAFETY_ANALYSIS` + 运行期断言，并注明理由。
- `disown_all_queues()` 全程持 pi 锁排空，让 `reset_owner()` 继续做唯一的摘链者，
  循环每轮重读表头。锁序是「线程 pi 锁在外、队列锁在内」，和 `WaitQueue::enqueue_waiter`
  的注释里写的方向一致。

否掉的方案：保留 swap、把 `reset_owner()` 拆成「摘链」和「清 owner」两个函数。这样确实能把
pi 锁的临界区压到一次 swap，但代价是 owner 字段和链表成员关系可以短暂地不一致，而
PI 传播恰恰要靠这两者一致来判断一条边在不在。排空一个将死线程的队列不是热路径，不值得
为它引入一个中间状态。

**参照**

- Linux `task_struct` 的 `pi_lock` / `alloc_lock` 分工，见 `include/linux/sched.h` 的成员注释、
  `task_lock()`（`include/linux/sched/task.h`）、以及 `rt_mutex_adjust_prio_chain()`
  （`kernel/locking/rtmutex.c`）里对 `pi_lock` 的用法。
- Zircon 这里没有对应物：它只有一把全局 `thread_lock`，`OwnedWaitQueue` 的 owner 变更全程在
  它下面，所以不存在「归哪把锁」的问题。本树不要全局 ThreadLock，因此必须自己划这条线。

**验证**

- `ninja -C out/bringup.x86` 全量通过。
- `owned_wait_queue.cpp` 这个 TU 的 `-Wthread-safety` 告警仍是 12 条，全部来自
  `thread.hpp` / `pm_node.hpp` 既有的积压（`m_flags`、`m_scheduling_context`、`m_stack`、
  `m_hard_affinity`），没有一条提到 `m_owned_wait_queues`；`thread.cpp` 的 31 条同理。
  见 `docs/repairs/260903/thread-safety.md`。
- `clang-format` 无改动。
- **QEMU 未实测到这条改动**：当前树能启动到时钟源切换，随后在
  `ours::task::sched::Fair::enqueue_thread`（`+0x1cf23`）撞 `#DE`，停在那里。那是 fair
  调度器里的除零，和本条无关；本条改的三个函数全树没有调用者，启动路径根本走不到。

**故意没做**

- 没有动 `m_flags`、`m_state`、`m_arch_thread`、`m_stack` 的归属。`m_flags` 声明归 resource 锁
  但 `detach()` 在 pi 锁下写它、`join()` 不拿锁读它；`m_state` 干脆没有 `TA_GUARDED`；
  `m_arch_thread` 标了 resource 锁而上下文切换根本不拿。这些每一条都要单独定，
  已记在 `docs/repairs/260903/thread-safety.md` 第 4.1 节。
- 没有动 `kernel/mm/pm_node.cpp` 里拿当前线程 resource 锁罩住整个页分配器那三处。
- 没有实现 ChainLock。本条只是把边集合挪到将来要变成 ChainLock 的那把锁下面，
  为它清出前提。
- 没有把 `OwnedWaitQueue` 从继承改成组合。那是独立的一条，牵动 `WaitQueue::as<Derived>()`
  和 `m_type` 判别，不该混进一条锁的改动里。
