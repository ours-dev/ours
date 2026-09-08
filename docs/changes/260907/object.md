# object 改动记录

## `ThreadDispatcher::spawn()` 丢掉了它创建的对象（2026-09-07）

**文件**
- `kernel/object/thread_dispatcher.cpp` — `spawn()` 写出参、构造函数初始化成员、地址空间在这里接好

**症状**
潜伏，直到有第一个调用者为止；那时是 use-after-free，不是编译错误。

```cpp
auto ThreadDispatcher::spawn(ustl::Arc<ProcessDispatcher> process, char const *name,
                             KernelHandle<Self> *out) -> Status {
    auto self = ustl::Arc<Self>::adopt(new (mm::GAF_USER) Self(process, name));
    ...
    auto core_thread = task::Thread::spawn(
        name, [](void *arg) -> i32 { return Self::trampoline(static_cast<Self *>(arg)); },
        self.as_ptr_mut(), task::DEFAULT_PRIORITY);
    ...
    self->m_kernel_thread = core_thread;

    DISPATCHER_THREAD_SPAWN_COUNT.add(1);
    return Status::Ok;            // *out 从头到尾没有被写过
}
```

**原因**
`out` 从未被赋值，于是 `self` 是这个对象唯一的引用，而它在函数返回时析构——引用计数
归零，dispatcher 被释放。同一时刻 `task::Thread` 正揣着 `self.as_ptr_mut()`：那是它
trampoline 的 `arg`，也就是这个线程一跑起来就要解引用的指针。所以这不是"泄漏"，是
反过来的：对象死得太早，线程活着。

而且调用者拿回的是 `Status::Ok` 和一个空句柄，于是它连 `start()` 都调不了——
`start()` 是设置用户态入口并最终 `resume()` 的那一步。也就是说这个函数在写成的那天
起就无法被正确使用，只是没有人用过。

顺带发现构造函数把 `m_flags`、`m_state`、`m_parked_reason`、`m_kernel_thread` 全部
留作未初始化：

```cpp
ThreadDispatcher::ThreadDispatcher(ustl::Arc<ProcessDispatcher> process, char const *name)
    : Base(), m_process(process) {}
```

`m_flags` 最要紧——`mark_active()` 是 `m_flags |= Flags::Active`，读改写一个从没被
赋过值的字段，Active 位是或进分配器留下的任意内容里的。

**一条要收回的判断**：`docs/repairs/260906/object.md` 里我把
`activate()` 中的 `ustl::Arc<Self>::adopt(this)` 记成了"对同一对象二次 adopt，两份
所有权"。那是错的。`ustl::Rc` 的 adopt 构造函数会 `retain()`：

```cpp
explicit Rc(T *pointer) noexcept : m_pointer(pointer) { ... retain(); }
static auto adopt(T *pointer) noexcept -> Rc { return Rc(pointer); }
```

也就是说它等价于 Zircon 的 `fbl::RefPtr<T>(this)`（新增一个所有者），而**不是**
`fbl::AdoptRef`（接管一个已存在的 +1）。这里正需要新增一个所有者，所以那一行本来就是
对的。名字与行为相反是真的，所以在代码里留了注释——照字面理解读下去，结论就是
double-free。

**改法**

```cpp
    if (!process || !out) {
        return Status::InvArgs;
    }
    ...
    core_thread->set_aspace(self->m_process->aspace());
    self->m_kernel_thread = core_thread;

    *out = KernelHandle<Self>(ustl::move(self));
```

构造函数补齐初始化列表。

`set_aspace()` 从调用点搬到了这里：以前是 `userabi/userboot.cpp` 自己
`thread->set_aspace(...)`，那把一个 kernel/kernel 的细节交到了一个本该只看得见对象层
的调用者手上，而且只有在它恰好知道"必须在第一次 resume() 之前"时才对。放在这里，这个
顺序是结构性成立的：`resume()` 发生在 `activate()` 里，而 `activate()` 要经过
`start()`，`start()` 又只能在这个函数返回句柄之后才被调到。顺序之所以要紧，是因为
调度器切入线程时按 `Thread::aspace()` 重装 CR3——给一个已经在跑的线程改 aspace，会让
记账和硬件对不上。

**参照**
- `Zircon ThreadDispatcher::Create()`：同样以 `KernelHandle<ThreadDispatcher>*` 出参
  交出对象，同样在 `Initialize()` 里把内核线程与 dispatcher 互相接好。
- `Zircon fbl::RefPtr<T>(T*)` 对 `fbl::AdoptRef(T*)`：ustl 的 `adopt` 语义是前者。

**验证**
`ninja` 通过；QEMU 实测两个镜像，userboot 由 `ThreadDispatcher::spawn()` +
`start()` 启动并进入 ring 3，30 秒内 910 次 `Exception[interrupt] from user space`，
无缺页、无 panic。这是这个函数的第一个调用者。

**故意没做**
- `resume()` / `suspend()` 仍然只 `verify_canary()` 什么也不做；`on_exit()` /
  `on_resume()` / `on_suspend()` 同样是空壳。它们要等挂起/恢复真的有语义时再写，
  现在填内容只能是猜。
- `name` 仍然被丢掉：`ThreadDispatcher` 没有名字成员（它把名字转交给了
  `task::Thread`）。`ProcessDispatcher` 这次加了 `m_name`，线程这边等有查询接口
  （类似 `zx_object_get_property(ZX_PROP_NAME)`）时再说。
