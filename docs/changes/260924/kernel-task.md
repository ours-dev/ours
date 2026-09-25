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

## 可 join 线程改为明确不支持：`exit()` 报错而不是停机，`join()` 返回 `NotSupported`（2026-09-24）

**文件**
- `kernel/kernel/task/thread.cpp` — `exit()` 的可 join 分支改 `panic`；`join()` 返回 `Status::NotSupported`，注释列出缺什么；`try_wake_joiners()` 返回 `true`；删掉不再用的 `<arch/halt.hpp>`
- `kernel/include/ours/task/thread.hpp` — `join()` 声明处注明不支持
- `docs/repairs/260924/kernel-task.md` — 删掉第 7 条

**症状**
潜伏，今天够不到：每个会退出的线程都是 Detached（`spawn()` 的调用者都会 `detach()`，`bind_user_thread()` 给每个用户线程置 Detached），
也没有人调 `join()`。一旦有线程不 detach 就退出，这颗 CPU 关着中断、持着自己的 PI 锁 `hlt` 下去，不再醒；
之后任何 CPU 去拿这把 PI 锁都在它后面自旋。不 panic，不打日志。

**原因**
`exit()` 的可 join 分支是：

```cpp
while (!current_thread->m_task_state.try_wake_joiners(Status::Ok)) {
    arch::halt();
}
```

而 `try_wake_joiners()` 唤醒之后恒 `return false`。循环没有出口；关着中断的 `hlt` 也等不到能打断它的中断。

照抄 Zircon 的形状但抄错了两处：Zircon 的 `TryWakeJoiners()` 只在 chain lock 退让（backoff）时返回 false，唤醒成功就返回 true；
循环体是 `arch::Yield()`（自旋提示），不是 `hlt`。

只修返回值不够让 join 变对。和 `Zircon Thread::Join()` 对照，join 这条路还缺：

1. `join()` 把 `this`（被 join 的线程）挂到结果队列上，并断言 `this == Current`——等于线程 join 自己。Zircon 断言的正相反：`this != current_thread`，挂的是调用者。
2. "是否已死"的检查和挂起不是一步，`exit()` 的唤醒可以落在两者之间，join 的一方就永远睡下去。
3. `exit()` 对可 join 线程照样丢掉 PI 锁、排一个释放用的 Dpc；join 的一方也会 `destroy()`——释放两次，
   而且 join 的一方可能在 `exit()` 还跑在这个线程的栈上时就把它释放了。Zircon 在这里不排 Dpc，持着线程锁直接进最后一次调度（`!queue_free_dpc`）。
4. 没人把可 join 的线程从 `SPAWNED_THREADS` 摘下来（Zircon 在 `Join()` 里 `EraseFromListsLocked()`），`destroy()` 不摘。
5. `detach()` 对已死的线程在持 PI 锁时调 `join()`，`join()` 再拿同一把锁，自锁死。Zircon 的 `Detach()` 先放锁再 `Join()`。
6. 唤醒 joiner 要在退出线程的 PI 锁里再拿 joiner 的 PI 锁。本项目刻意没有全局 thread_lock，这种嵌套要靠 ChainLock 式的退让来避免死锁，线程锁现在还没有。

第 6 条决定了做对 join 不是这一条改动能完成的事；而今天没有任何用户态需求要 join。所以诚实的做法是明确地不支持。

**改法**
改前：

```cpp
        } else {
            while (!current_thread->m_task_state.try_wake_joiners(Status::Ok)) {
                arch::halt();
            }
        }
```

改后：

```cpp
        } else {
            panic("thread |{}| exited joinable, and join() is not supported: detach() it",
                  current_thread->name());
        }
```

`join()` 的函数体整个换成 `return Status::NotSupported;`，上面的注释逐条写了上面 1–6，给以后实现 join 的人当清单。
`try_wake_joiners()` 改成 `return true`：`notify_all()` 自旋拿锁、把队列清空，不会失败。它现在没有调用者，
但留着一个名为 try、却恒返回"失败"的函数，就是留着下一次同样的死循环。

被否掉的方案：让可 join 的线程退出后留作"尸体"（照 Zircon 不排 Dpc、持锁调度），只让 `join()` 不支持。
这样 exit 一侧和 Zircon 一致，但没有 join 就没有人回收，每个忘了 `detach()` 的线程都安静地泄漏一个 Thread 和一整个内核栈，
还留着第 3、4 条的半成品。panic 让"必须 detach"这条规则在第一次被违反时就被看见。

**参照**
- `Zircon Thread::Current::Exit()`（`kernel/kernel/thread.cc`）：可 join 分支 `while (TryWakeJoiners(ZX_OK) == false) arch::Yield();`，之后不排 `free_dpc`，持锁 `RescheduleInternal()`。
- `Zircon TaskState::TryWakeJoiners()`：`AcquireOrBackoff()` 拿到队列锁才 `WakeAllLocked()`，结果非空即返回 true。
- `Zircon Thread::Join()`：`DEBUG_ASSERT(this != current_thread)`；在 list lock 与目标线程锁下检查 `THREAD_DEATH`，否则换成当前线程锁与队列锁去 `task_state_.Join(current_thread, ...)`；最后 `EraseFromListsLocked()`、`free_thread_resources()`。
- `Zircon Thread::Detach()`：已死的线程先 `guard.Release()` 再 `Join(nullptr, 0)`。

**验证**
- 构建通过；clang-tidy 在改动行上没有新告警。
- 两个镜像启动：dev 日志有 `init: shell says "pong"`、`shell: channel checks passed`、`shell: bad-pointer checks passed`；
  dbg_1 跑到 `--- end ---`，Detached 的 `laborer` 正常退出。
- 临时注释掉 `kernel/top/main.cpp` 里的 `laborer->detach()`（未提交，已还原）再启动 dbg_1：
  日志出现 `thread |laborer| exited joinable, and join() is not supported: detach() it`，内核 panic 停下，而不是无声地停机。

**故意没做**
- 没有实现 join，理由见上面第 6 条和"被否掉的方案"。`TaskState::join()` 与计数器 `thread.join` 留着，给以后的实现用。
- `detach()` 对已死线程的分支没改：现在可 join 的线程死不了，走到这个分支意味着线程已是 Detached，调用者手里的指针可能已被释放，
  那是调用者的错，不是这里能补的。它调 `join()` 也不再自锁死，因为 `join()` 不再拿锁。
- `TaskState::invoke()` 在入口函数返回后、线程还没 Dead 时就 `notify_all()` 一次，实现 join 时要么删掉、要么让 joiner 醒来后重查状态。今天没有 joiner，不动。

## 杀或挂起一个在别的 CPU 上运行的线程，发一个中断 IPI（2026-09-24）

**文件**
- `kernel/kernel/task/thread.cpp` — `suspend_or_kill()` 的 Running 分支：目标不在本 CPU 就 `IpiService::send_event(cpu, IpiEvent::Interrupt)`；`process_pending_signals()` 的注释改正
- `kernel/arch/x86/kernel/syscall.S` — `call x86_syscall_process_pending_signals` 上方的注释改正
- `docs/repairs/260924/kernel-task.md` — 删掉第 8 条

**症状**
潜伏。今天唯一的 kill 是线程在 fault 路径上杀自己。一旦有别的 CPU 去杀（P1-B 的 `task_kill`、多线程进程被杀），
目标若在 ring 3 里跑一个不做系统调用的循环，而它那颗 CPU 上又恰好没有到期的定时器中断，Kill 就一直挂在 `m_signals` 里没人处理，
进程迟迟不死。P1-A 在 `syscall.S` 和 `process_pending_signals()` 里的注释原本假定会有 IPI 跟上，勘误里已改为"还不发"。

**原因**
信号只在线程从内核回 ring 3 的最后一段被检查：系统调用出口（`x86_syscall_process_pending_signals`）、
中断/异常出口（`arch_handle_exception()` 的 `from_user` 分支）、第一次进用户态。置了信号位而目标不进内核，检查就不会发生。

Running 分支原来只是 `break`：它依赖目标"很快"进内核，而这件事没人保证。Ready 分支同样只 `break`，但那是对的——
Ready 的线程下一次被调度上 CPU 时必然从内核出发回到 ring 3，路上会经过检查。

**改法**
改前：

```cpp
        case State::Running: {
            break;
        }
```

改后：

```cpp
        case State::Running: {
            auto const cpu = curr_cpu();
            DEBUG_ASSERT(cpu != INVALID_CPU_NUM, "Running thread {} belongs to no CPU", name());
            if (cpu != Cpu::id()) {
                IpiService::send_event(cpu, IpiEvent::Interrupt);
            }
            break;
        }
```

- 用 `IpiEvent::Interrupt`（`IrqVec::IpiInterrupt`，处理函数是空的），不用 `Resched`：要的只是让目标进一次中断，
  中断出口在 `from_user` 时会跑 `process_pending_signals()`。Resched 会额外置一个"待抢占"位，平白多一次调度。
  Zircon 这里用的也是 `mp_interrupt()`（`mp_ipi::INTERRUPT`），不是 reschedule。
- 目标就是调用者自己（本 CPU 上 Running 的只能是当前线程）时不发：它自己回 ring 3 的路上就会看到信号。
- 读 `curr_cpu()` 不会读到过时的值：一个 Running 的线程离开 CPU 只能经过一次调度，而每条调度路径都先拿它的 PI 锁，这里正持着。
  `Cpu::id()` 也不会变，Guard 关了抢占。调度器在 `set_next_thread()` 里写 `m_curr_cpu` 时还没拿新线程的 PI 锁，
  但从 B 的队列里挑出来的线程，入队时 `m_curr_cpu` 已经是 B。
- 几种落点都不丢：IPI 打在 ring 3 → 中断出口检查；打在内核里 → 离开内核那一段检查；打在回 ring 3 前已关中断的那一小段 →
  中断挂起，SYSRET/IRETQ 一开中断就在 ring 3 被取走，再走一次中断出口。

两处注释改为：另一颗 CPU 在窗口里置的信号不会漏，`suspend_or_kill()` 随后发的中断 IPI 在关中断期间挂起，回到 ring 3 时被取走，中断出口再检查一次。

**参照**
- `Zircon Thread::SuspendOrKillInternal()`：`case THREAD_RUNNING: mp_interrupt(mp_ipi_target::MASK, cpu_num_to_mask(scheduler_state_.curr_cpu_));`，
  注释说这一步"不是必需的，只是让挂起或杀掉更早发生，而不是等下一次时钟中断或系统调用"。
- `Linux signal_wake_up_state()` → `kick_process()`（`kernel/sched/core.c`）：`if ((cpu != smp_processor_id()) && task_curr(p)) smp_send_reschedule(cpu);`——
  同样只对"正在别的 CPU 上跑"的任务发，本 CPU 不发。

**验证**
- 构建通过；clang-tidy 在改动行上没有新告警。
- 两个镜像启动：dev 日志有 `init: shell says "pong"`、`shell: channel checks passed`、`shell: bad-pointer checks passed`；dbg_1 跑到 `--- end ---`。
- 临时实验（未提交，已还原）：在 `labour_routine()` 里起一个绑在 CPU 1 上自旋的内核线程，50 ms 后从 CPU 0 `kill()` 它，
  并在 `x86_handle_ipi_interrupt()` 里临时打一行日志。dbg_1：

  ```
  [4357634000 cpu0 info] EXPERIMENT: killing ipi-probe from CPU[0]: running true curr_cpu 1
  [4358153000 cpu1 info] EXPERIMENT: IPI-Interrupt on CPU[1]
  ```

  IPI 落在目标所在的 CPU 上，约 0.5 ms 后。dev 那次目标恰好是 Ready，按设计没有发。
  自旋的是内核线程，不回 ring 3，所以"Kill 在中断出口被处理、线程退出"这一段没有端到端测到；那要等有跨线程的 kill 调用者（P1-B）。

**故意没做**
- Ready 分支不发 IPI，理由见上：它下次上 CPU 必然经过检查。Zircon 同样只 `break`。
- 没有把 `x86_handle_ipi_resched()` 里自己调 `apic_issue_eoi()`、分发处又调一次的重复 EOI 一起改：那属于 x86-kernel，与本条无关。

## 删掉从没人写过的 `IrqNestedCounter` 和 `in_interrupt()`（2026-09-24）

**文件**
- `kernel/include/ours/task/preemption_state.hpp` — 删掉 `Tag::IrqNestedCounter`、它的 4 位字段和 `in_interrupt()`；`is_preemption_enabled()` 去掉 `!in_interrupt()` 这一项；在 `Tag` 上写明为什么这个字里不放中断计数；顺手改正位号注释（原来两个字段都标 `Bit 25`）
- `kernel/kernel/irq/mod.cpp` — `handle_irq_preamble()` 补文档注释：它是 Zircon 的 `int_handler_start()`，对所有向量都跑，故意不加 hard-IRQ 计数
- `kernel/lib/logz4/kernel.cpp` — `domain_slot()` 的注释原来引用 `IrqNestedCounter` 从不自增，改为指向 `Tag` 上的说明

**症状**
无可见症状。`IrqNestedCounter` 全树只有读和声明，没有一处增减，所以 `in_interrupt()` 恒为 false，
`is_preemption_enabled()` 里的 `!in_interrupt()` 永远不起作用。真正能回答"本 CPU 是否在处理中断"的
只有 `irq::current_frame()`，logz4 的 `domain_slot()` 正是因此绕开了 `in_interrupt()`。
（`docs/repairs/260903/scheduler.md` 第 4 条记的就是这件事。）

**原因**
这是一个"两份信号、只有一份活着"的问题。要么让它活，要么删掉，二选一；对照 Zircon 和 Linux 之后选了删：

1. **维护它什么也不会改变。** 这个计数唯一的读者是 `is_preemption_enabled()`，而中断前导已经把
   两个禁用计数都抬起来了：

   ```cpp
   auto handle_irq_preamble(HIrqNum irqnum, arch::IrqFrame *frame) -> void {
       mm::CpuLocal::write(CURRENT_FRAME, frame);
       auto &preemption_state = task::Thread::Current::preemption_state();
       preemption_state.disable_eager_reschedule();
       preemption_state.disable_preemption();
   }
   ```

   所以整个处理函数期间 `preemption_disabled_count() != 0`，`is_preemption_enabled()` 本来就是 false。
   `!in_interrupt()` 就算有人维护，也只会在"已经为 false"的地方再 false 一次。
   `PreemptionState` 照的是 Zircon（`PreemptReenableDelayFlush`、eager resched、timeslice extension、pending mask），
   而 Zircon 的 `PreemptIsEnabled()` 就是 `state_.load() == 0`，这个字里只有两个禁用计数和 timeslice extension 标志，
   没有中断计数。进中断时 `int_handler_start()` 做的也正是 `PreemptDisable()` + `EagerReschedDisable()`。

2. **Linux 要这几位的理由，ours 没有。** Linux 的 `preempt_count` 是全内核唯一回答
   "在 hardirq / softirq / NMI / 进程上下文"的字，`in_interrupt()` = `irq_count()` = NMI|HARDIRQ|SOFTIRQ 位。
   最直接的消费者是 `__irq_exit_rcu()`：`preempt_count_sub(HARDIRQ_OFFSET)` 之后 `if (!in_interrupt() && local_softirq_pending()) invoke_softirq();`
   ——它要知道自己是不是最外层的中断出口，才能决定跑不跑软中断。ours 没有软中断这一层，没有这个问题要回答。

3. **Linux 只给设备中断计数，不给异常计数；ours 的前导对所有向量都跑。** Linux 的 hard-IRQ 位是
   `irq_enter_rcu()` → `__irq_enter_raw()` → `preempt_count_add(HARDIRQ_OFFSET)` 加的，调用点在
   `arch/x86/include/asm/irq_stack.h`（设备中断和 sysvec 的路径）；#PF 走 `DEFINE_IDTENTRY_RAW_ERRORCODE(exc_page_fault)`
   → `irqentry_enter()`，不碰 HARDIRQ。ours 的 `arch_handle_exception()` 对除 NMI/#DF/#MC 外的每个向量都调
   `platform_start_handling_irq()`，#PF 也在里面。在前导里直接 `++` 就会把缺页算成"中断上下文"；要做对得先让前导
   区分向量——为一个没人读的字段。

4. **"是否在中断里"已经有一个按 CPU 的信号。** `irq::current_frame()` 由前导置、尾声清，logz4 和采样 profiler 在用。
   Zircon 对应的东西同样是按 CPU、且不在 `PreemptionState` 里：`int_handler_start()` 置的 `blocking_disallowed`。
   再在每线程的字里放一份，只会和它漂移——这一份已经漂成了死字段。

改前：

```cpp
    auto is_preemption_enabled() const -> bool {
        return preemption_disabled_count() == 0 && eager_resched_disabled_count() == 0 &&
               !in_interrupt() && !m_counter.get<Tag::NmiNestedCounter>() &&
               !m_counter.get<Tag::TimesliceExtension>();
    }
    ...
    auto in_interrupt() const -> bool {
        return m_counter.get<Tag::IrqNestedCounter>();
    }
    ...
                                     Field<Tag::IrqNestedCounter, 4, u8>,            // Bits 16..19
                                     Field<Tag::NmiNestedCounter, 4, u8>,            // Bits 20..23
                                     Field<Tag::NoReschedPreempt, 1, bool>,          // Bit 24
                                     Field<Tag::NoReschedEager, 1, bool>,            // Bit 25
                                     Field<Tag::TimesliceExtension, 1, bool>         // Bit 25
```

**改法**
改后：

```cpp
    auto is_preemption_enabled() const -> bool {
        return preemption_disabled_count() == 0 && eager_resched_disabled_count() == 0 &&
               !m_counter.get<Tag::NmiNestedCounter>() && !m_counter.get<Tag::TimesliceExtension>();
    }
    ...
                                     Field<Tag::NmiNestedCounter, 4, u8>,            // Bits 16..19
                                     Field<Tag::NoReschedPreempt, 1, bool>,          // Bit 20
                                     Field<Tag::NoReschedEager, 1, bool>,            // Bit 21
                                     Field<Tag::TimesliceExtension, 1, bool>         // Bit 22
```

`in_interrupt()` 整个删除，上面四条理由原样写进 `Tag` 的注释，读代码的人不必来翻这份记录。
字段删掉后后面的位整体前移 4 位；没有汇编或别处按位号读这个字（`raw_value()` 是私有的且无调用者），所以前移无害。

被否掉的方案：在 `handle_irq_preamble()` / `handle_irq_postamble()` 里增减计数（Linux `irq_enter`/`irq_exit` 式）。
理由见上：对唯一的读者零效果；要和 Linux 语义一致还得先区分异常与中断；而且会造出第二个和 `current_frame()` 平行的信号。

**参照**
- `Zircon int_handler_start()` / `int_handler_finish()`（`kernel/include/kernel/interrupt.h`）：只动禁用计数和 `arch_set_blocking_disallowed()`。
- `Zircon PreemptionState::PreemptIsEnabled()`（`kernel/include/kernel/thread.h`）：`state_.load() == 0`。
- `Zircon x86_exception_handler()`（`kernel/arch/x86/exceptions_c.cc`）：`arch_blocking_disallowed()` 为真即判"中断处理里递归"。
- `Linux irq_enter_rcu()` / `__irq_exit_rcu()`（`kernel/softirq.c`），`__irq_enter_raw()`（`include/linux/hardirq.h`），`in_interrupt()` / `irq_count()`（`include/linux/preempt.h`）。

**验证**
- `prebuilt/ninja/ninja -C out/bringup.x86` 通过。
- clang-format 已跑；clang-tidy 对 `irq/mod.cpp`、`logz4/kernel.cpp`（连带头文件）在改动行上无告警，只有 `mod.cpp` 两条原有的 `CURRENT_FRAME`/`IRQ_DISPATCHER` 命名告警。
- 两个镜像启动：
  - `kernel.dev.obi`：`init: shell says "pong"`、`shell: channel checks passed`、`shell: bad-pointer checks passed`、`--- end ---` 都在。
  - `kernel.dev.dbg_1.obi`：`shell: channel checks passed`、`shell: bad-pointer checks passed`、`--- end ---` 都在，无断言、无 panic。
    这个镜像里 shell 由内核直接起（"started by the kernel; no one to answer"），不经 init，所以本来就没有 `init: shell says "pong"`。

**故意没做**
- `NmiNestedCounter` 处境完全相同（只读不写；NMI 路径按设计不做任何簿记），但任务只涉及 IRQ 计数，没有一并删，留在 repairs。
- Zircon 的 `blocking_disallowed`（在可能睡眠的锁里断言"不在中断里"）ours 还没有，另记在 repairs；它不属于 `PreemptionState`，
  而且 ours 的 #PF 目前在前导/尾声之间、关抢占关中断地处理，照搬之前得先像 Zircon `x86_pfe_handler()` 那样在缺页路径里放开。

## 删掉 `NmiNestedCounter`，并写明嵌套中断的计数该放哪（2026-09-24）

**文件**
- `kernel/include/ours/task/preemption_state.hpp` — 删掉 `Tag::NmiNestedCounter` 和它的 4 位字段，`is_preemption_enabled()` 去掉对应一项；
  `Tag` 上的注释扩成两个计数一起说，并加一段"嵌套中断服务的计数该放哪"；位号注释随之前移

**症状**
无可见症状。和上一条的 `IrqNestedCounter` 一样：只被 `is_preemption_enabled()` 读，全树没有一处写，恒为 0。

**原因**
NMI 路径按设计不做任何簿记（`arch_handle_exception()` 的注释：NMI、#DF、#MC 直接进 `x86_dispatch_exception()`，
不跑前导/尾声）。理由是 NMI 可以落在任何地方，包括 IRQ 前导做到一半的时候，它不能去动底下那段代码正在改的状态。
而这个字段唯一可能的写入点恰恰就是 NMI 路径——要维护它，就得推翻那条设计。Zircon 的 `x86_nmi_handler()`
同样不碰 `PreemptionState`，只做 `kcounter_add` 和 `g_cpu_context_exchange.HandleRequest()`。

Linux 确实在 `__nmi_enter()` 里 `__preempt_count_add(NMI_OFFSET + HARDIRQ_OFFSET)`，给 `in_nmi()` 用
（printk、perf、tracing、lockdep 靠它避开 NMI 可能打断的锁）。ours 真需要"是否在 NMI 里"时，那也是一个按 CPU 的信号，
由 NMI 路径自己置，不属于每线程的抢占字。

改前：

```cpp
        return preemption_disabled_count() == 0 && eager_resched_disabled_count() == 0 &&
               !m_counter.get<Tag::NmiNestedCounter>() && !m_counter.get<Tag::TimesliceExtension>();
    ...
                                     Field<Tag::NmiNestedCounter, 4, u8>,            // Bits 16..19
                                     Field<Tag::NoReschedPreempt, 1, bool>,          // Bit 20
                                     Field<Tag::NoReschedEager, 1, bool>,            // Bit 21
                                     Field<Tag::TimesliceExtension, 1, bool>         // Bit 22
```

**改法**
改后：

```cpp
        return preemption_disabled_count() == 0 && eager_resched_disabled_count() == 0 &&
               !m_counter.get<Tag::TimesliceExtension>();
    ...
                                     Field<Tag::NoReschedPreempt, 1, bool>,          // Bit 16
                                     Field<Tag::NoReschedEager, 1, bool>,            // Bit 17
                                     Field<Tag::TimesliceExtension, 1, bool>         // Bit 18
```

**关于嵌套中断服务（计划中）为什么也不要这两个计数。** 这是删之前被问到的问题，答案写进了 `Tag` 的注释：

1. 嵌套是 CPU 的属性，不是线程的。任何处理函数还在栈上时线程都切不走，所以每线程的计数只会是按 CPU 状态的镜像。
2. 抢占这一面，禁用计数本身就会嵌套：内层处理函数退出时看到外层前导抬起的计数还在，`is_preemption_enabled()` 为假，
   只有最外层出口会接手挂起的抢占。Linux 的同一效果来自 HARDIRQ 位：`raw_irqentry_exit_cond_resched()` 只在
   `!preempt_count()` 时 `preempt_schedule_irq()`，内层出口时外层的 HARDIRQ 还在，所以不会调度。ours 里扮演这个角色的是
   `PreemptionDisabledCounter`，不需要另一个计数。
3. 嵌套真正会弄坏的是 `CURRENT_FRAME`：内层尾声写 null，外层还在跑却以为自己不在中断里（NMI 曾经这样坏过，见
   `arch_handle_exception()` 的注释）。要做的是前导保存被替换的帧、尾声还原而不是写 null——Zircon 的
   `int_handler_saved_state_t` 对 `blocking_disallowed` 就是这么做的；需要问"嵌套了几层"时，深度也放在 `kernel/irq`
   里 `CURRENT_FRAME` 旁边，按 CPU。

**参照**
- `Zircon x86_nmi_handler()`（`kernel/arch/x86/exceptions_c.cc`）：不碰 `PreemptionState`。
- `Zircon int_handler_start()` / `int_handler_finish()`（`kernel/include/kernel/interrupt.h`）：保存再还原 `blocking_disallowed`，不是直接置假。
- `Linux __nmi_enter()`（`include/linux/hardirq.h`）；`raw_irqentry_exit_cond_resched()`（`kernel/entry/common.c`）。

**验证**
- `prebuilt/ninja/ninja -C out/bringup.x86` 通过；clang-format 已跑；clang-tidy（经 `irq/mod.cpp` 带入头文件）无新告警。
- 两个镜像启动：`kernel.dev.obi` 有 `init: shell says "pong"`、`shell: channel checks passed`、`shell: bad-pointer checks passed`、`--- end ---`；
  `kernel.dev.dbg_1.obi` 有后两行和 `--- end ---`，无断言、无 panic（这个镜像不经 init，没有 pong 行，同上一条）。

**故意没做**
- 没有顺手把 `CURRENT_FRAME` 改成保存/还原：嵌套中断还没开，现在改是为不存在的调用者写代码；等做嵌套时和开中断、IRQ 栈一起做。
