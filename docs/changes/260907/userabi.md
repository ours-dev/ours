# userabi 改动记录

## userboot 的线程也走对象层，手写的入口线程随之消失（2026-09-07）

**文件**
- `kernel/lib/userabi/userboot.cpp` — 删掉全局镜像记录与 `enter_userboot()`，改用 `ThreadDispatcher`

**症状**
无。这是把最后一处 `kernel/kernel` 直接调用换掉。

**原因**
上一轮之后，userboot 的进程、地址空间、内存、映射都走了对象层，只剩线程还是手搓的：

```cpp
auto *const thread =
    task::Thread::spawn("userboot", &enter_userboot, nullptr, task::DEFAULT_PRIORITY);
...
thread->set_aspace(loaded.process.dispatcher()->aspace());
thread->detach();
thread->resume();
```

而 `enter_userboot()` 自己搭中断帧进 ring 3：

```cpp
[[noreturn]]
auto enter_userboot(void *) -> i32 {
    auto const &image = USERBOOT_IMAGE;
    arch::IrqFrame frame{};
    arch_build_uspace_irqframe(&frame, image.entry, image.stack_top, 0, 0);
    arch_enter_uspace(&frame);
}
```

这段代码和 `ThreadDispatcher::trampoline()` 是逐行重复的——后者早就存在，做的是同一件
事。区别只在于 trampoline 从 dispatcher 的 `m_user_entry` 读入口点，而这里从一个
**文件作用域变量**读。那个全局变量的存在理由也正是这个：入口线程是一个裸内核线程，
除了全局变量之外没有别的办法把值从 `userboot_init()` 送到一个稍后才运行的函数里。

**改法**

```cpp
object::KernelHandle<object::ThreadDispatcher> thread;
status = object::ThreadDispatcher::spawn(process.dispatcher(), "userboot", &thread);
if (Status::Ok != status) { ... }

status = thread.dispatcher()->start({
    .entry = loaded.entry,
    .stack = loaded.stack_top,
    .arg1 = 0,
    .arg2 = 0,
});
```

`start()` 而不是 `resume()`：内核线程只需要被变成可运行的，用户线程还得被告知从用户
空间的哪里开始。这也正是将来 `sys_thread_start` 的参数表。

连带删掉的：

- `enter_userboot()` 整个函数（`ThreadDispatcher::trampoline()` 就是它）；
- 文件作用域的 `USERBOOT_IMAGE`——`LoadedImage` 变成了 `userboot_init()` 里的普通局部
  变量，因为入口点现在由 `start()` 作为参数交给 dispatcher 保存；
- `thread->set_aspace(...)`，搬进了 `ThreadDispatcher::spawn()`（见
  `docs/changes/260907/object.md`）；
- `thread->detach()` / `thread->resume()`——`Thread::bind_user_thread()` 自己会置
  `Flags::Detached`，`activate()` 自己会 resume；
- `#include <ours/arch/entry.hpp>` 和 `#include <ours/task/thread.hpp>`。

净效果是这个文件少了 25 行，并且**不再直接触碰 `ours::task` 的任何东西**。

生命周期这次是一条完整的链，函数结束时所有句柄都能安全析构：内核线程持有
ThreadDispatcher，ThreadDispatcher 持有 ProcessDispatcher，ProcessDispatcher 拥有
地址空间，映射活在地址空间里。VMA 与 VMO 的 dispatcher 是"被允许创建这些映射的对象"，
不是"持有映射的东西"，所以它们先走并不影响任何东西。

**参照**
`Zircon userboot`：同样是 `ThreadDispatcher::Create()` 之后 `Start()`，入口点与栈顶
作为参数传入，没有任何一处手搭中断帧。

**验证**
QEMU 实测两个镜像：

```
[info] userboot: mapped [200000, 203000) into its own aspace, entry at 201270, stack 1200000
```

进入 ring 3 后 30 秒内 910 次 `Exception[interrupt] from user space`，无缺页、无 panic，
两个镜像都以 signal 15 结束。

**故意没做**
- **`//kernel/mm` 依赖还在**，只剩 `get_frames_n()` 与 `PhysMap`，卡在
  `mm::VmObjectPaged::write()` 未实现（`docs/repairs/260906/mm.md` 第 3 条）。
- **句柄表仍然没有**，所以 userboot 仍然是"被对象搭出来的"，而不是"拿到了对象"。
- **还是没有系统调用**：userboot 的全部内容仍是一个 `pause` 自旋循环。
