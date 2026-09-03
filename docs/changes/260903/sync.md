# sync / lockdep — 改动记录

格式见 `docs/changes/TEMPLATE.md`。

---

## 1. 让 `Guard` 真正消费 `StateSaver`：`IrqSave` 从此真的关中断（2026-09-03）

**文件**
- `system/lib/lockdep/include/lockdep/state_saver.hpp` — 修 `StateSaver` 别名里的逗号表达式。
- `system/lib/lockdep/include/lockdep/guard.hpp` — 让 `Guard` 持有每个选项对应的 saver。
- `kernel/include/ours/lockdep.hpp` — `ours::IrqSave` / `ours::PreemptDisable` 改成
  `lockdep` 同名 tag 的 using，不再是自己新声明的空结构体。
- `kernel/include/ours/sync/spin_lock.hpp` — saver 声明改写成模板，并挪进 `spinlock`
  命名空间；补上组合选项。
- `kernel/include/ours/sync/local_lock.hpp` — 补 `LocalLock` + `IrqSave` 的 saver 声明。
- `kernel/kernel/time/clock_event_device.cpp` — 去掉 `Guard<sync::Mutex, IrqSave>` 上的
  `IrqSave`。

**症状**

全树每一处 `Guard<sync::SpinLock, IrqSave>` 都是一把光秃秃的自旋锁，中断始终开着。
凡是「线程上下文拿锁 + 中断处理里拿同一把锁」的地方都可能同 CPU 自死锁。

可观察的那一次：debug 镜像停在 `Scheduler::reschedule_common()` 的
`DEBUG_ASSERT(arch::irqs_disabled())`，从 `Thread::Current::preempt()` 进来——那里的
`Guard<SpinLock, IrqSave>` 本该已经把中断关掉了。

**原因**

三个独立的缺陷叠在一起，任何一个单独修都不会让这套机制工作起来。

1. **`StateSaver` 别名是个逗号表达式。** 它原本写成

   ```cpp
   using StateSaver = decltype(lockdep_get_state_saver(static_cast<Lock *>(nullptr)),
                               static_cast<SavedStateContainer<Option> *>(nullptr));
   ```

   第一个实参后面的右括号把整个 `decltype` 的内容变成了逗号表达式，类型取右操作数——
   于是无论什么锁、什么选项，别名一律是 `SavedStateContainer<Option> *`，
   一个空 tag 结构体的指针，什么也关不掉。而且那个调用只传了一个实参，
   和 `LOCKDEP_STATE_SAVER` 声明的两参函数根本对不上。没人发现，是因为这个别名没有用户。

2. **`Guard` 从不读它的选项。** 成员是 `std::tuple<bool, Options...>`，存的是选项 **tag**
   本身——空结构体——而 `guard()` 只有一句 `m_lock.acquire()`。所以选项是惰性的。

3. **调用点传的根本不是那套 tag。** `kernel/include/ours/lockdep.hpp` 里有

   ```cpp
   namespace ours { struct IrqSave {}; struct PreemptDisable {}; }
   ```

   两个全新的空结构体，和 `lockdep::IrqSave`（`SavedStateContainer<...>`）同名而无关。
   `namespace ours` 里的非限定查找先找到它们，所以全树的 `Guard<SpinLock, IrqSave>`
   实例化出来的是 `ours::IrqSave`——一个 saver 机制从没听说过的类型。
   这一条是最隐蔽的：即使 1 和 2 都修好，映射照样匹配不上。

**改法**

1. `StateSaver` 改成真正的两参调用，并把参数从 `SavedStateOption` 非类型参数改成 tag 类型，
   与调用点的写法对齐。
2. `Guard` 用 `std::tuple<StateSaver<LockType, Options>...> m_savers` 持有 saver，
   `bool m_unlocked` 单独拿出来。顺序由语言保证且正是需要的：成员在构造函数体之前初始化，
   所以 saver 全部就位（中断已关）才 `acquire()`；析构函数体先跑，释放锁，之后才销毁成员，
   所以中断在锁放掉之后才恢复。`SpinLock::release_irq_restore()` 的注释讲的是同一条约束——
   先恢复中断会留下一个窗口，本 CPU 的中断可以去抢本 CPU 还持着的锁。
3. `ours::IrqSave` / `PreemptDisable` 改为 `using lockdep::...`，并补上
   `IrqSaveAndPreemptDisable`。
4. `spin_lock.hpp` 里的 saver 声明**必须挪进 `namespace spinlock`**。`StateSaver` 靠
   ADL 找它们，而 ADL 为 `SpinLock<Options>` 关联的是**类模板自己所在**的命名空间
   `ours::sync::spinlock`，不会向外层走。放在外层的 `ours::sync` 里能通过编译，
   然后在每个使用点报 `use of undeclared identifier 'lockdep_get_state_saver'`。
   顺手改写成模板，一条声明同时覆盖 `SpinLock` / `MonitoredSpinLock` / `NoTraceSpinLock`。
5. 组合选项的 saver 用 `std::tuple<AutoInterruptDisabler, AutoPreemptDisabler>`，
   不新造类。`std::tuple` 逆序析构正是这里要的：先放开抢占，此时中断仍是关的——
   `reschedule_common()` 入口断言的就是这个状态——中断最后才恢复。

`Guard<sync::Mutex, IrqSave>`（`clock_event_device.cpp`）去掉了 `IrqSave`。
`sync::Mutex` 争用时会把调用者挂到 `OwnedWaitQueue` 上，关着中断去拿一把会阻塞的锁，
CPU 就再也回不来了。这个 tag 以前不起作用，所以去掉它**今天不改变任何行为**，
只是不让机制生效后把它变成一个真的 bug。

否掉的方案：给 `Mutex` 也声明一个 `IrqSave` 的 saver。那等于把「睡眠锁可以关中断持有」
写进接口里。

**参照**

- Zircon `lockdep::Guard` 与 `LockPolicy<LockType, Option>`：saver（Zircon 叫 policy state）
  是 Guard 的成员，构造顺序保证它先于 `Acquire()`。
- Linux 的对应物是 `spin_lock_irqsave()` / `spin_unlock_irqrestore()` 宏对，
  把状态放在调用者栈上；本树的 `SpinLock::acquire_irq_save()` 是同一形状，
  `Guard` 是它的 RAII 版本。

**验证**

- 全量 `ninja -C out/bringup.x86` 通过。
- QEMU，debug 镜像（`kernel.dev.dbg_1.obi`）：**报告的那条断言没有了**。整个启动日志里只剩
  一条断言，而且是更靠后的另一条——`Assertion failed on 0 == m_runtime.curr_thread`。
  启动现在能走过 tick device 选择、进到线程创建（两个内核栈已分配），
  停在调度器 bring-up 上。那条是 kernel/task 那边的事。
- 非 debug 镜像启动到 `CPU[0] keeps tick device HPET over LVT` 后 QEMU 自行退出，
  和本条改动之前一样，没有变化。

**故意没做**

- 没有去查 `0 == m_runtime.curr_thread` 那条新断言。它在别人的车道上。
- 没有清点「现在中断真的关了」之后可能暴露出来的时序变化。全树约二十处
  `Guard<..., IrqSave>` 的运行时行为都变了——这是它们本来就该有的行为，
  但确实是一次范围不小的行为改变，值得留意。
- 没有给 `Mutex` 补 saver，理由见上。
- `system/lib/lockdep` 的两个文件**提交不进去**：它是未初始化的子模块，
  和 `system/lib/concurrent` 一样。本次提交只含内核侧的四个文件。
