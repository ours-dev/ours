# kernel/task 与 arch 接缝 — 改动记录

格式见 `docs/changes/TEMPLATE.md`。

---

## 1. 上下文切换的 arch 接缝收 `Thread *`，不再收 `ArchThread *`（2026-09-12）

**文件**
- `kernel/arch/x86/kernel/include/ours/arch/thread.hpp` — `X86Thread::switch_thread` 签名。
- `kernel/arch/x86/kernel/task/thread.cpp` — 实现。
- `kernel/kernel/task/thread.cpp` — 调用点不再窄化。

**症状**

无。签名没说实话而已。

**原因**

```cpp
static auto switch_thread(Self *prev, Self *next) -> void;   // Self = X86Thread

auto X86Thread::switch_thread(Self *prev, Self *next) -> void {
    x86_set_tss_sp(Thread::of(next)->kernel_stack().top());   // ← 第一行就绕回去了
    ...
}
```

参数声明成 `ArchThread *`，而函数干的第一件事是 `Thread::of(next)` 用 container_of 把它
**绕回** `Thread *`——因为它需要的 `kernel_stack()` 长在 `Thread` 上。调用方先把
`Thread *` 窄化成 `&thread->m_arch_thread`，被调方再还原回去，一来一回什么都没换到。

窄化也不构成任何隔离：`Thread` 自己声明了 `friend ArchThread`（`thread.hpp:446`），
arch 那一半一直就能够到它的外层 `Thread`。签名把这条依赖藏起来，只是让读代码的人更难看见。

**改法**

让签名说实话：

```cpp
static auto switch_thread(Thread *prev, Thread *next) -> void;

auto X86Thread::switch_thread(Thread *prev, Thread *next) -> void {
    auto &prev_arch = prev->m_arch_thread;
    auto &next_arch = next->m_arch_thread;

    x86_set_tss_sp(next->kernel_stack().top());              // 不再 Thread::of
    swap_gsbase_and_fsbase(next_arch.m_gs_base, ..., &prev_arch.m_fs_base);
    x86_switch_context(&prev_arch.m_sp, next_arch.m_sp);
}
```

`Thread::of(ArchThread *)` 这个重载**保留**——`X86Thread::init()` 还在用
（`Thread::of(this)`，那里确实只有 `this`）。

**参照**

`Linux switch_to(prev, next, last)` 收的是 `struct task_struct *`，不是某个 arch 子结构；
`Zircon Thread::switch_thread` 同样。arch 接缝按线程说话，不按它自己那一半说话。

**故意没做**

- **没有顺手去掉 `m_runtime.prev_thread`**。这是当初讨论的起点，结论是它和本条无关：
  值要跨过栈切换，通道只有「汇编返回的寄存器」或「双方都看得见的内存」两条，而
  `switch_thread` 的参数是**换出方**压进去的——恢复方从自己的栈上醒来，读到的 `prev`
  是它当初睡下去时的那个，不是「刚才在我之前跑的人」。传 `Thread *` 下去改变不了这一点。

  真要走 Linux 那条路，得给 `x86_switch_context` 多一个参数并在末尾 `mov %rdx, %rax`
  ——靠的是「`rsp` 换了、参数寄存器没换」这个性质。收益是省掉一个字段，代价是一条跨架构的
  ABI 约定；而且**省不掉它本来要省的那个陷阱**：`complete_context_switch()` 还要
  `set_state(State::Idling, ...)`，那是恢复方 CPU 的状态，`Current::get()` 跑不掉。
  已评估并否掉。

- **汇编仍然只认两个栈指针**。让 `context.S` 直接收 `Thread *` 需要一个手工维护的
  `m_sp` 偏移常量，那是给汇编增加对 C++ 布局的依赖——现在它对 `Thread` 一无所知，
  这是它最好的性质。

**验证**

构建通过；`clang-tidy` 无新告警（`x64ContextSwitchFrame` 那条命名告警是既有的，在 `init()` 里）；
QEMU dbg 四核启动无回归：无断言、17.6 万行线程输出。
