# 原子量迁移到 `std::atomic`

格式见 `docs/changes/TEMPLATE.md`。

---

## 1. `ustl::sync::Atomic` 全量迁移为 `std::atomic`（2026-09-12）

**文件**
- 35 个文件：`kernel/include/ours/sync/{spin_lock,mutex,rwlock,seqlock}.hpp`、
  `kernel/include/ours/task/{idle_thread,preemption_state}.hpp`、`koid.hpp`、
  `kernel/mm/**`、`kernel/object/**`、`kernel/lib/gktl/include/gktl/counter.hpp` 等。

**症状**

无。`ustl::sync::Atomic` 是 `__atomic_*` 内建函数的一层薄封装，能用；问题是它和标准库
重复，而本仓库的既定方向是「freestanding 安全的 `std::` 优先于 ustl 的自造包装」。

**原因与对应关系**

| 之前 | 之后 |
|---|---|
| `ustl::sync::Atomic<T>` | `std::atomic<T>` |
| `ustl::sync::AtomicU32` 等十个别名 | `std::atomic<u32>` 等 |
| `ustl::sync::AtomicRef` | `std::atomic_ref` |
| `ustl::sync::MemoryOrder::Relaxed` | `std::memory_order_relaxed`（其余同理） |
| `ustl::sync::atomic_{signal,thread}_fence` | `std::atomic_{signal,thread}_fence` |
| `#include <ustl/sync/atomic.hpp>` | `#include <atomic>` |

迁移能这么直接，是因为两边的形状本来就对齐——尤其 `compare_exchange_weak/strong` 两边
都收 `T &expected`，所以**所有调用点一个字都不用改**：

```cpp
// 之前
if (!m_lockval.compare_exchange_strong(old_state, holder, MemoryOrder::Acquire,
                                       MemoryOrder::Relaxed)) {
// 之后
if (!m_lockval.compare_exchange_strong(old_state, holder, std::memory_order_acquire,
                                       std::memory_order_relaxed)) {
```

`MemoryOrder` 的成员本来就是 `__ATOMIC_RELAXED` 这些内建常量，和 `std::memory_order`
的取值一一对应，语义没有任何变化。

**顺带修好的一件事**

`ustl::sync::Atomic` 的默认构造是 `= default`，**不初始化存储**；`std::atomic` 自 C++20
起是值初始化的。`kernel/mm/page_queues.cpp` 的构造函数注释原本写着：

> ustl::sync::Atomic's default constructor is `= default`, which leaves its storage
> indeterminate, so a PageQueues that is not in .bss would start out claiming an
> arbitrary number of live generations.

这条隐患随迁移消失，注释已改成说明那几个显式的零现在是「表达意图」而非「修补类型」。

**载荷类型**

`std::atomic<T>` 要求 `T` 平凡可复制。逐个核对过非标量的用法：`CpuMask`（`BitSet`，一个字）、
`IdleThread::StateMachine`（两个枚举，`alignas(2)`）、`Duration`、以及若干枚举——全部满足。

**故意没做**

- **没有删 `system/lib/ustl/include/ustl/sync/atomic.hpp`**。内核侧已无使用者，但 ustl 是
  独立的 freestanding 库，可能有本仓库之外的消费者；删不删是 ustl 自己的决定，记进待修。
- 没有借机改动任何内存序。这次迁移是**逐字对应**的，`Relaxed` 仍是 relaxed；哪里的内存序
  该不该收紧是另一回事，混进来就没法从 diff 上看出哪些是纯迁移。

**验证**

构建通过；QEMU 两个镜像实测：dbg 四核无断言、14.2 万行线程输出；dev 无异常且存活。
prof 宿主单测全过（6 + 4）。
