# kernel-task 改动记录（2026-09-24）

## `Thread::Current::kill()` 的断言条件写反了（2026-09-24）

**文件**
- `kernel/kernel/task/thread.cpp` — `Thread::Current::kill()` 的 `DEBUG_ASSERT` 取反

**症状**
潜伏，当前没有调用者。一旦有线程调用它来杀自己（P1-A 的 fault-kill 会走到同一类路径），
dbg_1 镜像会对每一个普通线程断言失败；唯一能通过断言的却是 idle 线程，而它恰恰是
唯一不能被杀的。

**原因**
断言的消息写的是"试图杀 idle 线程"，条件却是 `is_idle_thread()` 为真。`DEBUG_ASSERT`
在条件为假时触发，所以它实际表达的是"当前线程必须是 idle"。消息和条件说的是相反的
两件事，读的人照消息理解，编译器照条件执行。

idle 线程不能死，是因为它是 CPU 在没有别的可跑时运行的东西：调度器在空队列上总能
挑到它，杀掉它之后，这颗 CPU 下一次无事可做时就挑不出任何线程。

**改法**
改前：

```cpp
    DEBUG_ASSERT(current_thread->is_idle_thread(), "Try to kill the idle thread");
```

改后：

```cpp
    DEBUG_ASSERT(!current_thread->is_idle_thread(), "Try to kill the idle thread");
```

**参照**
`Zircon Thread::Current::Kill()`：`DEBUG_ASSERT(!current_thread->IsIdle())`。
`Thread::Current::Exit()` 同样断言 `!IsIdle()`。

**验证**
构建通过；两个镜像启动正常。没有调用者，所以这条断言本身未被执行到。

**故意没做**
没有给 `Thread::Current::exit()` 补同样的断言：P1-A 计划第 20 条要在 `exit` 入口加
中断与抢占状态的断言，届时一起加，避免同一个函数被改两次。

## `suspend_or_kill` 关中断、关抢占后再拿 PI 锁（2026-09-24）

**文件**
- `kernel/kernel/task/thread.cpp` — `Thread::suspend_or_kill()` 的 Guard 加上 `IrqSaveAndPreemptDisable`

**症状**
潜伏，当前没有调用者。P1-A 的 fault-kill 让线程杀自己之后，这里会在一次时钟中断上自锁死：
CPU 停在自旋里，不 panic，也不再打任何日志。

**原因**
`Guard thread_guard{get_pi_lock()}` 由推导指引变成 `Guard<SpinLock>`，不带任何选项——
它只拿锁，不关中断也不关抢占。

线程对自己调用 `suspend_or_kill` 时，持有的是**自己的** PI 锁。这时来一个时钟中断，
中断出口走 `Thread::Current::preempt()`，它要拿的正是当前线程的 PI 锁。同一颗 CPU、
同一把锁、没有第二个参与者，自旋永远等不到释放。只要持锁期间可能被中断，而中断路径
又可能拿同一把锁，这把锁就必须在关中断下持有——这是自旋锁最基本的规则，与锁是谁的无关。

旁边的 `resume()` 已经用 `Guard<SpinLock, lockdep::IrqSaveAndPreemptDisable>`；
`suspend_or_kill` 是漏掉的那个。关抢占也需要：持锁期间被抢占，下一个线程在同一颗 CPU
上去拿这把锁，结局一样。

**改法**
改前：

```cpp
    Guard thread_guard{get_pi_lock()};
```

改后：

```cpp
    Guard<SpinLock, lockdep::IrqSaveAndPreemptDisable> thread_guard{get_pi_lock()};
```

**参照**
- `Linux try_to_wake_up()`：`raw_spin_lock_irqsave(&p->pi_lock, flags)`。本项目 Thread 的两把自旋锁照 Linux 建模。
- `Zircon Thread::SuspendOrKillInternal()`：整个事务在 `ChainLockTransaction::UntilDone(PreemptDisableAndIrqSaveOption, ...)` 下执行。

**验证**
构建通过；两个镜像启动正常。没有调用者，改动本身未被执行到。

**故意没做**
同文件里还有几处 `Guard guard{get_pi_lock()}`（`do_suspend`、`exit` 等），没有一并改。
P1-A 计划第 20 条会逐个看：有的调用点已经在外层关了中断，只加 Guard 选项不一定对，
要按各自的上下文判断，不适合在这一条里批量改。

## `process_pending_signals` 的 Kill 分支让线程退出（2026-09-24）

**文件**
- `kernel/kernel/task/thread.cpp` — `process_pending_signals()` 重写；`exit()` 入口加两条断言；`m_signals` 的三处写入改 `ktl::write_once`
- `kernel/include/ours/task/thread.hpp` — `signals()` 改 `ktl::read_once`
- `docs/repairs/260924/kernel-task.md` — 新增 `do_suspend` 一项

**症状**
潜伏。一个线程一旦被置上 Kill，回 ring 3 的路上进入 `process_pending_signals()`，Kill 分支是空的，
信号永远不清，`while` 在关中断的状态下永远转下去：这颗 CPU 死锁，没有任何输出。

**原因**
这个函数的循环以"信号清空"为出口，每个分支都得让自己的那一位消失：Suspend 分支在 `do_suspend()`
里清；Kill 分支什么都没做；未知位同样没人清。一个分支不处理自己的位，循环就没有出口。

**改法**
改前：

```cpp
    ThreadSignals signals;
    while ((signals = current_thread->signals()) != ThreadSignals::None) {
        if (!!(signals & ThreadSignals::Kill)) {
        }

        if (!!(signals & ThreadSignals::Suspend)) {
            arch::local_irq_enable();
            Thread::Current::do_suspend();
            arch::local_irq_disable();
        }

        if (!!(signals & ThreadSignals::None)) {
        }
    }
```

改后：

```cpp
    DEBUG_ASSERT(arch::irqs_disabled(), "{}() entered with interrupts on", __func__);

    ThreadSignals signals;
    while ((signals = current_thread->signals()) != ThreadSignals::None) {
        if (!!(signals & ThreadSignals::Kill)) {
            arch::local_irq_enable();
            Thread::Current::exit(0);
        }

        if (!!(signals & ThreadSignals::Suspend)) {
            arch::local_irq_enable();
            Thread::Current::do_suspend();
            arch::local_irq_disable();
            continue;
        }

        panic("thread {} has signals {:#x} nobody handles", ...);
    }
```

- Kill 先于 Suspend：一个既被挂起又被杀的线程没有可以恢复进去的东西。
- 退出前开中断：`exit()` 里 ThreadDispatcher 的 `on_exit()` 要拿会睡眠的锁。
- 未知位直接 panic，而不是再写一个空分支让循环空转。
- 删掉 `signals & ThreadSignals::None` 那个空分支（与 None 按位与恒为零，条件永假）——主人同意删除。

`exit()` 入口断言中断开、抢占开：它的调用者（线程入口返回后的 trampoline、上面两条信号路径）都
承诺能阻塞，断言把这个承诺写死。没有断言"不持自旋锁"：`SpinLock::num_held_cpu()` 存在，但 PI 锁会
在上下文切换时交接，这个每 CPU 计数是否平衡没有核实过。

`m_signals` 由别的 CPU 在 PI 锁下写，由线程自己在回用户态的路上不加锁地读，所以读写都改成
`ktl::read_once`/`ktl::write_once`：不撕裂、不被编译器缓存或推迟。

**参照**
- Zircon `Thread::Current::ProcessPendingSignals()`（`kernel/kernel/thread.cc`）：先看 Kill，
  `arch_enable_ints(); Thread::Current::Exit(0);`；Suspend 分支挂起后再关中断。
- Zircon `Thread::Current::Exit()`：用户线程在 `ExitingCurrent()` 前 `DEBUG_ASSERT(!arch_ints_disabled())`。
- Zircon 的 `signals_` 是 relaxed 原子量。

**验证**
- 构建通过；两个镜像在 `SMP=4` 与 `SMP=1` 下都跑到 `shell: bad-pointer checks passed` 与 `--- end ---`。
- dbg_1 日志里内核线程 `laborer` 走 trampoline 退出一次，`exit()` 的两条新断言成立。
- Kill 分支本身这一条还走不到：没有代码置 Kill。第 26 条（用户态异常杀进程）之后实测。

**故意没做**
`do_suspend()` 的锁（见待修项），P1-A 不会置 Suspend。

## 勘误：P1-A 第 1–16 条对抗审查查出的记录错误（2026-09-24）

本文件只增不改，更正写在这里。

- 「`suspend_or_kill` 关中断、关抢占后再拿 PI 锁」一节的**症状**写"CPU 停在自旋里，不 panic，也不再打任何日志"。
  这只对 dbg_0 成立。dbg_1（`KERNEL_DEBUG_LEVEL=1`）里，`Thread::Current::preempt()` 在中断出口对同一把锁的
  第二次获取会触发 `SpinLock::acquire()` 的 `DEBUG_ASSERT(!is_held())`（`kernel/kernel/sync/spin_lock.cpp`），
  内核 panic。
- 同一个函数还有两处早于 P1-A 的缺陷（Parked 分支重复加锁、Sleeping 分支与定时器竞争），以及
  `Thread::Current::tid()` 读错每 CPU 槽，已记入 `docs/repairs/260924/kernel-task.md` 第 2–4 条并交接。

## 勘误：P1-A 第 17–29 条对抗审查查出的记录错误（2026-09-24）

本文件只增不改，更正写在这里。

- 「`process_pending_signals` 的 Kill 分支让线程退出」一节写 `m_signals` "由别的 CPU 在 PI 锁下写"。不完全：
  `suspend_or_kill()` 里的 `gktl::defer` 动作会执行两次（`DeferredAction::invoke()` 不标记已执行），第二次在 PI 锁释放之后，
  是一次锁外的读改写。已交接给独立会话（与 `SPAWNED_THREADS_LOCK`、可 join 线程的退出循环、Kill 不发 IPI 一起）。
- 同一节写 `process_pending_signals()` "关中断，所以检查之后到来的信号不会在返回前被错过"。只对本 CPU 成立；另一颗 CPU
  在这个窗口里置的信号不会丢，但要等下一次进内核，因为还不发 IPI。

## `gktl::DeferredAction::invoke()` 让动作最多跑一次（2026-09-24）

**文件**
- `kernel/lib/gktl/include/gktl/defer.hpp` — `invoke()` 先置 `m_invoked` 再调用；删掉 const 重载；禁止拷贝；成员改 `m_` 前缀
- `kernel/lib/gktl/test/defer_test.cpp` — 新增主机测试，数动作跑了几次
- `kernel/lib/gktl/BUILD.gn` — `defer-test` 与 `tests`；`defer.hpp` 列进 `headers` 的 `public`
- `BUILD.gn` — 默认组带上 `//kernel/lib/gktl:tests($host_toolchain)`
- `docs/repairs/260924/kernel-task.md` — 删掉第 5 条

**症状**
每次 `Thread::suspend_or_kill()` 都会把 Kill/Suspend 位或进 `m_signals` 两次。第一次在目标线程的 PI 锁下；
第二次来自 `~DeferredAction`，在锁已释放、中断已恢复、抢占已重新打开之后。dbg_1 的 vmours 反汇编里
能看到 `popfq` 之后还有一条不在锁里的 `orl`。P1-A 的 fault-kill 每杀一次都走这条路。

**原因**
`invoke()` 调了动作，却不记下"已经调过"；析构函数也调 `invoke()`，于是动作再跑一遍。
`cancel()` 只是把同一个标志置位——它能拦住析构，恰恰说明标志的意思是"动作已花掉"，而 `invoke()` 忘了花。

第二次为什么有害，而不只是多余：`set_signals` 在 `thread_guard` **之前**声明，析构顺序与声明相反，
所以它比锁活得久，第二次运行落在锁外。`m_signals | Kill` 是读—改—写三步：读到旧值，或上一位，写回。
这三步之间另一颗 CPU 可以在锁下置 Suspend（或 Kill），锁外这次写回会用旧值把它冲掉。
`ktl::write_once` 只保证那一次存储不撕裂、不被推迟，保证不了读和写之间没人插进来——那是锁的事，
而这一次恰好不在锁里。

const 重载有同样的缺陷而且修不好：const 对象记不下自己跑过，通过它 `invoke()` 只能意味着
"现在跑一次，出作用域再跑一次"。可拷贝也是同一类问题：拷贝出来的对象带着自己那份待执行的动作，
两个对象各自析构时各跑一次。

**改法**
改前：

```cpp
    auto invoke() -> void {
        if (!invoked_) {
            invocable_();
        }
    }

    auto invoke() const -> void {
        if (!invoked_) {
            invocable_();
        }
    }
```

改后：

```cpp
    DeferredAction(Self const &) = delete;
    auto operator=(Self const &) -> Self & = delete;

    auto invoke() -> void {
        if (m_invoked) {
            return;
        }
        m_invoked = true;
        m_invocable();
    }
```

- 标志在调用**之前**置位，和 fit 的 `call()` 先清空 target 再调用一样：动作如果回头对自己的
  `DeferredAction` 调 `invoke()`，看到的是已花掉，而不是递归再跑。
- 禁止拷贝后 `defer()` 照样按值返回：它返回的是纯右值，C++17 保证直接构造进调用者的变量，不经拷贝。
  没有加移动构造——目前没有人需要把一个 defer 挪出作用域，加了就得处理"被移走的那个不能再跑"。
- 成员 `invoked_`/`invocable_` 改成 `m_invoked`/`m_invocable`：这几行本来就要改，`.clang-tidy` 要求私有成员带 `m_`。

树里另外两个用户：`kernel/mm/object_cache.cpp` 的 `MemCache::create()` 只用 `cancel()` 和析构，
不调 `invoke()`，行为不变；`scheduler.hpp`/`scheduler.cpp` 里提到 `gktl::defer` 的只是注释，
那段代码早已拆掉。`suspend_or_kill()` 本身不用改：早调 `invoke()` 的写法是对的，错的是 `invoke()`。

**参照**
- `fit::deferred_action::call()`（Fuchsia `sdk/lib/fit/include/lib/fit/defer.h`）：先把 target 挪到局部变量并
  `reset()`，再调用；拷贝构造与拷贝赋值 `= delete`。
- Linux 没有对应的 RAII 设施；最接近的是 `include/linux/cleanup.h` 的 `__cleanup`，它没有"提前执行"这个操作，
  也就没有这个问题。

**验证**
- `defer-test`（主机）通过；把 `HEAD` 的旧 `defer.hpp` 换进去编同一个测试，
  `test_invoke_runs_once` 断言失败（`count.runs == 1`，实际 2）——测试确实能抓住这个错。
- 构建通过；clang-tidy 对 `defer_test.cpp`（连带 `defer.hpp`）只剩模板参数 `Invocable_` 的命名一条，改动前就有。
- 两个镜像启动：dev 日志有 `init: shell says "pong"`、`shell: channel checks passed`、`shell: bad-pointer checks passed`；
  dbg_1 跑到 `--- end ---`。
- 反汇编 dbg_1 vmours 的 `suspend_or_kill`：对 `m_signals`（`0x98(%rbx)`）的 `orl` 只剩一条，在锁内；`popfq` 之后直接是返回。

**故意没做**
- 没给 `defer()` 加 `[[nodiscard]]`（fit 有 `warn_unused_result`）：丢掉返回值会让动作立刻执行，是另一类错，
  不属于"最多跑一次"。
- 模板参数 `Invocable_` 的命名告警是旧的，与本条无关，没动。

## `SPAWNED_THREADS_LOCK` 一律关中断获取（2026-09-24）

**文件**
- `kernel/kernel/task/thread.cpp` — `create_first()`、`spawn_etc()`、`erase_from_global_list()` 的 Guard 加 `lockdep::IrqSave`
- `kernel/include/ours/task/thread.hpp` — 锁的声明处写明"必须关中断"及原因
- `docs/repairs/260924/kernel-task.md` — 删掉第 6 条

**症状**
潜伏，窄窗口。`spawn_etc()` 持锁时本 CPU 来一个时钟中断并抢占它，接着本 CPU 上一个 Detached 线程
（每个用户线程都是，`bind_user_thread` 置的）走 `exit()` 去摘链：dbg_0 关着中断永远自旋；
dbg_1 在 `SpinLock::acquire()` 的 `DEBUG_ASSERT(!is_held())` 上 panic。P1-A 的 fault-kill 让用户线程退出变得常见。

**原因**
一把自旋锁只要有一个持有者在关中断的状态下拿它，所有持有者就都得关中断拿。

`exit()` 在 `AutoInterruptDisabler` + `AutoPreemptDisabler` + 自己的 PI 锁之下调 `erase_from_global_list()`，
这一处天然是关中断的。另外两处（`spawn_etc`、`create_first`）用的是 `Guard guard{...}`，推导成
`Guard<SpinLock>`——只拿锁，中断和抢占都不动。于是：

1. 线程 A 在 `spawn_etc()` 里拿到锁，中断开着；
2. 时钟中断，出口处 `preempt()` 把 A 换下，A 带着锁回到本 CPU 的运行队列；
3. 线程 B 在本 CPU 上 `exit()`，关中断、关抢占，去拿同一把锁，自旋。

A 只能被本 CPU 再调度（`Scheduler::steal_work_from()` 还是桩，没有别的 CPU 会把它拿走），
而本 CPU 关着中断在自旋，永远不会再调度。和 `dpc.cpp` 里 `DpcRunner::Queue::enqueue()` 修过的是同一个形状。

只关中断就够，不需要再关抢占：本项目的抢占只发生在中断出口和主动 `reschedule`，
关了中断的临界区里两者都不会发生。

**改法**
改前（三处相同）：

```cpp
    Guard guard{SPAWNED_THREADS_LOCK};
```

改后：

```cpp
    Guard<SpinLock, lockdep::IrqSave> guard{SPAWNED_THREADS_LOCK};
```

`erase_from_global_list()` 的调用者已经关了中断，那里的 IrqSave 只是保存再恢复同一个"关"，
写上是为了让"这把锁总是关中断拿"在每个获取点都成立，不依赖调用者。
原因写在锁的声明处，三个获取点各自只写一句指过去。

**参照**
- `Zircon Thread::CreateEtc()`：`Guard<SpinLock, IrqSave> list_guard{&list_lock_}` 之后才 `thread_list->push_front(t)`；
  `ConstructFirstThreadSecondaryCpu` 同样以 `Guard<SpinLock, IrqSave>` 持有 list lock。
- `Linux copy_process()` 与 `release_task()`（`kernel/fork.c`、`kernel/exit.c`）：全局任务链由 `tasklist_lock` 保护，
  写侧一律 `write_lock_irq(&tasklist_lock)`。

**验证**
- 构建通过；clang-tidy 在改动行上没有新告警（`SPAWNED_THREADS`/`SPAWNED_THREADS_LOCK` 的命名告警改动前就有）。
- 两个镜像启动：dev 日志有 `init: shell says "pong"`、`shell: channel checks passed`、`shell: bad-pointer checks passed`；
  dbg_1 跑到 `--- end ---`，Detached 的内核线程 `laborer` 经 `exit()` → `erase_from_global_list()` 正常退出。
- 抢占落在 `spawn_etc` 临界区里的那个竞争窗口没有专门构造，未实测。

**故意没做**
- 没有给 `SPAWNED_THREADS` 加 `TA_GUARDED(SPAWNED_THREADS_LOCK)`：两者都是 static 成员，声明顺序是链表在前、锁在后，
  要加得调换顺序并处理 `create_first()` 那样早期的调用点，是另一件事。
- `exit()` 里 PI 锁 → `SPAWNED_THREADS_LOCK` 的嵌套顺序与 Zircon（list lock 在前、线程锁在后）相反。
  今天没有反向嵌套的路径，所以不构成死锁；以后若有人在持 `SPAWNED_THREADS_LOCK` 时去拿某个线程的 PI 锁
  （比如遍历线程表的调试命令），就得先定顺序。
