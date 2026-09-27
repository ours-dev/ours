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
