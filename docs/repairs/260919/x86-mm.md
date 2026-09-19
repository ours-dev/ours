# x86/mm 待修项（2026-09-19）

## 1. 没有 TLB shootdown，`m_active_cpus` 是个没人用的空壳

`kernel/arch/x86/mm/include/ours/mm/arch_vm_aspace.hpp:48,63`：

```cpp
    auto active_cpus() const -> CpuMask;
    ...
    CpuMask m_active_cpus;
```

全树只有三处提到它：这两行声明，和构造函数里的 `, m_active_cpus()`。
`active_cpus()` 没有定义，`m_active_cpus` 没有任何一处写入或读取。

它要承担的事也确实没人做。全树的 TLB 失效只有 `kernel/arch/x86/mm/mmu.cpp:29`
调用的 `arch::tlb_invalidate_all()`，而那只刷本 CPU。一个地址空间同时挂在多个 CPU
上时，解映射或降权限之后，别的 CPU 的 TLB 里仍然留着旧翻译——已经释放的物理页
可以继续被另一个核读写。四核是默认配置，所以这不是理论问题。

这是 `m_active_cpus` 本来的用途：Linux 的 `mm_cpumask(mm)` 和 Zircon 的
`X86ArchVmAspace::active_cpus_` 都是同一个东西，用来决定把 invalidation IPI 发给谁，
而不是无条件广播给所有核。

要做的是一整条链，不是补一个函数：上下文切换时维护这个掩码、一个携带
（aspace, 虚拟地址区间）的 IPI、接收端的 `invlpg`/CR3 重载，以及等待确认的同步点。
按这个规模它应该单独一个会话做，`m_active_cpus` 不要因为「没人用」被删掉——它是这件
事的脚手架。
