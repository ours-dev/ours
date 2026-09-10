# mm / task — 地址空间归属

格式见 `docs/changes/TEMPLATE.md`。

---

## 1. 「空」不再是「内核地址空间」的写法（2026-09-10）

**文件**
- `kernel/mm/include/ours/mm/vm_aspace.hpp` — `kernel_aspace()` 改回裸指针。
- `kernel/mm/vm_aspace.cpp` — 删掉它的线外定义。
- `kernel/kernel/task/thread.cpp` — `spawn_etc()` 给新线程装上内核 aspace。
- `kernel/kernel/task/init.cpp` — `init()` 补装 boot 线程。
- `kernel/arch/x86/mm/arch_vm_aspace.cpp` — 兜底改成 panic。

**症状**

潜伏。`Thread::m_aspace` 为空的线程一直能正常跑，因为切换时有人替它兜底。

**原因**

```cpp
// arch_vm_aspace.cpp
if (to != nullptr) {
    ...Cr3::write(to_pgd);
} else {
    // Default to kernel aspace
    Cr3::write(KERNEL_PGD);
}
```

于是「空」同时表示两件在使用现场**完全分不开**的事：

1. 这是个内核线程（Zircon 的用法）；
2. 还没有人给这个线程装地址空间（ours 的实情——`task::init_early()` 建 boot 线程时
   `init_vmm()` 还没跑，那一刻根本没有任何地址空间可指）。

第 2 种是 bug，却被第 1 种的兜底悄悄补成了一个看起来正确的答案。这和本仓库刚refactor掉的
`VmasFlags` catch-all 是同一个形状，只是低了一层。

**改法**

让每个线程都持有真实的地址空间，「空」只剩「没装」一个含义：

```cpp
// thread.cpp, spawn_etc()
self->m_aspace = mm::VmAspace::kernel_aspace();

// init.cpp, init() —— boot 线程早于 VMM，只能事后补装
Thread::Current::get()->set_aspace(mm::VmAspace::kernel_aspace());

// arch_vm_aspace.cpp
} else {
    panic("switching to a thread with no address space");
}
```

顺带把 `kernel_aspace()` 从返回 `Arc` 改成返回裸指针：

```cpp
// 之前
auto VmAspace::kernel_aspace() -> ustl::Arc<VmAspace> {
    return ustl::Arc<VmAspace>::adopt(KERNEL_ASPACE);
}
// 之后（头文件内联）
static auto kernel_aspace() -> VmAspace * { return KERNEL_ASPACE; }
```

`Arc::adopt` 是**接管所有权而不加引用**，所以旧写法每次调用都造一个临时 Arc，析构时给一个
从没为它加过计数的单例减一次；没出事只是因为内核 aspace 永不释放。全树 16 个调用点没有一个
需要那份所有权，全是 `kernel_aspace()->...` 用完即弃。

**参照**

`Zircon` 相反：`RelaxedAtomic<VmAspace*> aspace_{nullptr}`（`kernel/include/kernel/thread.h:1419`），
只有 `ThreadDispatcher` 和单测会 `VmAspace::AttachToThread()`，普通内核线程一辈子是空；
`X86ArchVmAspace::ContextSwitch()`（`arch/x86/mmu.cc:1033`）看见空就写
`root_page_table_phys`。这里**有意不跟**：Zircon 那个约定要求读代码的人记住「空 = 内核线程」，
而它和「还没初始化」长得一模一样。

**验证**

构建通过；QEMU dbg 四核实测，无断言、无 panic、9.2 万行线程输出。

**故意没做**

- `Thread::Current::fault()` 仍然绕过 `m_aspace` 去问 `m_user_thread->process().aspace()`，
  于是「这个线程在哪个地址空间」有两处真相来源：缺页按进程解析，CR3 按 `m_aspace` 切换。
  现在碰不到是因为还没有真正的用户线程。已记进待修。
