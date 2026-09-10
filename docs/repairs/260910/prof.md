# kernel/lib/prof — 待修

---

## 1. 栈窗口的守卫无法被测试

`kernel_stack_read()` 和 `StackWindow` 在 `src/sampler.cpp` 的匿名命名空间里。这是整个库
里最需要测试的一段——它是唯一直接处理不可信输入（可能损坏的帧指针）的地方，而
changes 第 1 条刚在这里修掉一个整数溢出。

从外部够不着：端到端触发要构造真实的 FDE 索引，让展开器至少走完一步并读一次内存。

方向：把 `StackWindow` 提成 `prof/` 下一个具名类型，带 `above(sp)` 和 `contains(address)`，
让它自己守住自己的不变量，然后直接单测 `contains()` 的边界（顶端回绕、下界、对齐）。
