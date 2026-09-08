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

## 2. `ProcessDispatcher::create_shared()` 只有声明没有定义

`create()` 已经实现（见 `docs/changes/260906/object.md`），`create_shared()` 没有。
它要的共享地址空间语义——两个进程共享用户半区的上半段，`SHARED_ASPACE_BASE` /
`SHARED_ASPACE_SIZE` 那一对常量就是为它留的——目前没有任何调用者，签名之外的部分
只能靠猜。等到有需求时再写。

