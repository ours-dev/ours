## OwnedWaitQueue 的通知钩子改用 ustl::function::Function（2026-09-27）

**文件**
- `kernel/include/ours/task/owned_wait_queue.hpp`：`INotifyRequeueHook` 的三个成员改为 `Function`
- `kernel/include/ours/task/types.hpp`：删掉没人用的 `<ustl/function/fn.hpp>`

**症状**
潜伏，当前触发不到。`allow`、`on_notify`、`on_requeue` 原来都是 `std::function`，
由 `lock_for_notify_operation()` 在队列的自旋锁下调用。现在唯一的实例
`DEFAULT_WAKE_HOOK` 装的是三个不捕获任何东西的 lambda，不会分配。但只要以后
有调用方传一个捕获较多的钩子，构造它时 `std::function` 就会调用 `operator new`，
而这个内核的全局 `operator new` 只是一个 panic 桩。

**原因**
自旋锁下被调用的回调必须保证构造和调用都不分配，`std::function` 给不出这个
保证。`Function` 把上限（24 字节）写进了类型约束，超了就编译不过。

**改法**

之前：

```cpp
struct INotifyRequeueHook {
    ustl::function::Fn<auto()->bool> allow;       // = std::function
    ...
};
```

之后：

```cpp
struct INotifyRequeueHook {
    ustl::function::Function<auto()->bool> allow;
    ...
};
```

`DEFAULT_WAKE_HOOK` 的指定初始化器不用改：lambda 会隐式转换成 `Function`。
钩子一直按引用传递，从来没有被拷贝过，所以"只能移动"对它没有影响。

`types.hpp` 里的线程入口早就是"函数指针 + 上下文指针"了，那行 include 是遗留，
一并删掉。

**参照**
Zircon `OwnedWaitQueue::IWakeRequeueHook`，它是一个带虚函数的接口类。
这里保留了原来的"三个可调用对象"形状，只换了容器。

**验证**
- `kernel.dev.obi` 和 `kernel.dev.dbg_1.obi` 均构建通过。
- QEMU 实测：dev 启动 45 s，dbg_1 启动 60 s，都无 panic，两个镜像的
  `usercopy.test` 都是 18/18。Mutex 的释放慢路径会经过 `default_wake_hook()`，
  但没有专门确认启动过程中这条路径被触发过。

**故意没做**
`CpuStopper::Fn` 还是 `std::function`，这次没动。`stop_cpus()` 要把同一个 fn
分给多个 CPU 的 `m_stop_work`，这要求 fn 能拷贝，而 `Function` 只能移动。
这一处怎么改需要先定设计，单独处理。

## CpuStopper::worker_thread() 的 work 指针每轮重置（2026-09-27）

**文件**
- `kernel/kernel/task/cpu_stopper.cpp`：`worker_thread()` 里的 `work` 声明挪进循环

**症状**
潜伏，当前触发不到：CpuStopper 还没有接上（没有创建 worker，也没有调用方）。
一旦接上，第一个 work 跑完、队列变空之后，worker 不会退出，而是再跑一遍同一个
work，而且会一直这样跑下去。

**原因**
`Work *work = nullptr;` 写在 `while` 外面，循环里只在队列非空时给它赋值。队列空了，
`work` 仍然指向上一个 work，`if (!work) break;` 永远不成立。

更糟的是那个 work 已经失效了。`stop_cpu()` 的 `Work` 是调用方栈上的局部变量，
worker 执行 `done->completion.signal()` 之后，调用方就从 `wait()` 返回，栈帧随之
回收。再跑一遍就是读已经释放的栈内存，还会再对一个已经不存在的 `Event` 调一次 `signal()`。

**改法**

之前：

```cpp
Work *work = nullptr;
while (1) {
    { /* 队列非空时 work = front */ }
    if (!work) { break; }
```

之后：

```cpp
while (1) {
    Work *work = nullptr;
    { /* 队列非空时 work = front */ }
    if (!work) { break; }
```

**参照**
Linux `cpu_stopper_thread()`：每轮都从 `work = NULL` 开始，再在
`stopper->lock` 下从 `stopper->works` 取。

**验证**
`kernel.dev.obi` 和 `kernel.dev.dbg_1.obi` 均构建通过。这段代码还不可达，所以启动验证
说明不了什么，没有专门测。

**故意没做**
没有把 CpuStopper 接上：谁来创建 worker、`m_state` 谁来初始化，都属于把它启用起来的工作。

## CpuStopper 的 Fn 改用 ustl::function::Function，删掉 stop_cpus_nowait()（2026-09-27）

**文件**
- `kernel/include/ours/task/cpu_stopper.hpp`：`Fn` 改为 `Function`；删掉 `stop_cpus_nowait()`；`m_fn` 改为指针；补全各接口的注释
- `kernel/kernel/task/cpu_stopper.cpp`：`stop_cpu()`、`stop_cpu_nowait()` 把 fn 移进 Work；`queue_work_cpus()` 给每个 CPU 装一个转发 lambda；`worker_thread()` 改为通过指针读 Work

**症状**
潜伏，当前触发不到：CpuStopper 还没有接上。有两个问题：
- `Fn` 是 `std::function`，而 Work 是在 `m_lock` 下、关着中断入队的，目标一大就会分配内存。
- `stop_cpu_nowait()` 只在自己分配 Work 时才填 fn。调用方自己传进来的 Work，
  入队时里面是什么 fn 就跑什么 fn。

**原因**
第一个问题的道理和 Timer、OwnedWaitQueue 相同：锁下调用的回调必须保证不分配。

换成 `Function` 之后，唯一麻烦的是 `stop_cpus()`：它要把同一个 fn 分给多个 CPU。
原来是每个 CPU 的 `m_stop_work` 各拷一份 `std::function`，而 `Function` 只能移动，
拷不了。

**改法**

`stop_cpus()` 的 fn 留在它自己的栈帧上，每个 CPU 的 Work 里放一个转发 lambda，
只捕获一个指向它的指针：

```cpp
// 之前
cpus.for_each([&is_queued, fn, done](CpuNum cpu) {
    std::construct_at(&stopper.m_stop_work, fn, caller_pc(), done);

// 之后
cpus.for_each([&is_queued, &fn, done](CpuNum cpu) {
    auto &work = stopper.m_stop_work;
    work.fn = [shared = &fn] { return (*shared)(); };
    work.caller = caller_pc();
    work.done = done;
```

`stop_cpus()` 会等所有 CPU 跑完才返回，所以指针在整个过程中都有效。
代价是这个 fn 会在多个 CPU 上并发地、通过同一个目标被调用，所以它必须经得起并发。
Linux 的 `stop_cpus()` 把同一对 fn 和 arg 交给所有 CPU，是同样的约定，注释里写明了。

顺带把 `construct_at` 覆盖旧 Work 改成了逐字段赋值。旧 Work 里的 fn 现在是
`Function`，赋值会正确析构它，直接覆盖则跳过了析构。

`stop_cpu_nowait()` 无论 Work 是不是调用方给的，都填 fn、caller、done。

`worker_thread()` 以前用 `auto [fn, caller, done, _] = *work;` 把 Work 拷出来，
Work 现在不可拷贝，所以改成通过指针读取。`signal()` 之后不再碰 `work`，
因为 `stop_cpu()` 的调用方此时可能已经返回，把 Work 连同栈帧一起带走了。
`m_fn` 随之从 `Fn` 改为 `Fn const *`，只用于诊断。

删掉 `stop_cpus_nowait()`。原实现分配了 `works` 却不用，所有 CPU 共用同一个
`m_stop_work`。更根本的问题是，一个只能移动的 fn 没法分给多个 CPU，
又没有一个能活过所有 CPU 的栈帧可以托管它。Linux 现在也没有这个接口。

**参照**
Linux `stop_one_cpu()`、`stop_one_cpu_nowait()`（负责填 `work_buf`）、`stop_cpus()`，
以及 `struct cpu_stopper` 里用于诊断的 `fn` 和 `caller`。

**验证**
- `kernel.dev.obi` 和 `kernel.dev.dbg_1.obi` 均构建通过；clang-tidy 在改动行上无新告警。
  `STOP_CPUS_MUTEX` 的命名告警是原有的。
- QEMU 实测：两个镜像都无 panic，`usercopy.test` 都是 18/18。CpuStopper 还不可达，
  所以这次启动只说明没有弄坏别的东西。

**故意没做**
没有把 CpuStopper 接上，也就是没做：创建 worker、初始化 `m_state`、定义它的 CpuLocal 实例。
