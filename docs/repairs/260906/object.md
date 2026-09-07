# object 待修项（2026-09-06）

## 1. `KernelHandle::reset()` 调用了不存在的 `Dispatcher::on_zero()`

```cpp
template <std::derived_from<Dispatcher> U>
auto reset(ustl::Arc<U> dispatcher) -> void {
    if (m_dispatcher) {
        m_dispatcher->on_zero();   // Dispatcher 上的函数叫 on_zero_handles()
    }
    m_dispatcher = ustl::move(dispatcher);
}
```

`reset()` 是成员模板，至今没有任何调用者，所以从未被实例化，错误也就从未暴露。
第一个调用它的人会拿到一个编译错误。

需要先想清楚的不只是改名：`on_zero_handles()` 的语义是"最后一个句柄关闭了"，
而 `reset()` 换掉的是一个**内核态**引用（KernelHandle 还没进任何句柄表），
这两件事不是一回事。很可能正确的做法是这里根本不该通知 dispatcher。

## 2. `ProcessDispatcher::create()` / `create_shared()` 只有声明没有定义

`kernel/object/process_dispatcher.cpp` 里没有它们的实现，`userabi/userboot.cpp`
里那处调用被注释掉的历史版本也印证了这一点。链接期才会发现。
这是 userboot 走通对象层的下一步，已在认领中。

## 3. `ThreadDispatcher::spawn()` 丢掉了它创建的对象

```cpp
auto self = ustl::Arc<Self>::adopt(new (mm::GAF_USER) Self(process, name));
...
self->m_kernel_thread = core_thread;
DISPATCHER_THREAD_SPAWN_COUNT.add(1);
return Status::Ok;              // *out 从未被写入
```

`out` 参数从头到尾没有被赋值，而 `self` 在函数结束时析构——引用计数归零，对象被
释放，可 `task::Thread` 那边还揣着它的裸指针（`trampoline` 的 `arg`）。
另外 `activate()` 里又写了一次 `ustl::Arc<Self>::adopt(this)`，对同一个对象做了
第二次 adopt，两份所有权。

这两条要一起改，属于 userboot 接对象层的第 4 步。
