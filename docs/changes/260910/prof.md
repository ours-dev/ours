# kernel/lib/prof — 改动记录

格式见 `docs/changes/TEMPLATE.md`。

---

## 1. 栈读取的边界检查会被整数溢出绕过（2026-09-10）

**文件**
- `kernel/lib/prof/src/sampler.cpp` — `StackWindow` 与 `kernel_stack_read()`。

**症状**

潜伏。当前触发不到，因为要凑齐两件事：采样中断打在内核态，且展开器解出一个接近地址空间
顶端的帧指针。一旦发生，就是**中断上下文里的野读**。

**原因**

```cpp
struct StackWindow final { u64 lo; u64 hi; };

auto kernel_stack_read(void *context, u64 address, u64 *out) -> bool {
    auto const *window = static_cast<StackWindow const *>(context);
    if (address < window->lo || window->hi < address + 8) {   // ← 这里
        return false;
    }
    *out = *reinterpret_cast<u64 const *>(address);
    return true;
}
```

`address` 是展开出来的**帧指针**——这个窗口存在的全部理由就是不信任它（原注释：
"so a corrupt frame pointer cannot turn into a wild kernel read"）。而 `address + 8`
会回绕：取 `address = 0xFFFF'FFFF'FFFF'FFFC`，`address + 8` 变成 `0x4`，
`hi < 4` 为假；同时它远大于 `lo`，两个条件都过，于是解引用了那个地址。

窗口的构造同病：

```cpp
StackWindow window{ .lo = context.sp, .hi = context.sp + STACK_WINDOW };
```

`context.sp` 来自被中断 CPU 的陷阱帧，同样不可信；`sp` 接近顶端时 `hi` 回绕成一个很小的
值，之后**所有**上界检查都形同虚设。

**改法**

比较改写成不构造和式的形式，并把窗口构造收进一个饱和的具名构造函数：

```cpp
static_assert(STACK_WINDOW >= sizeof(u64),
              "the window must hold at least one slot, or hi - sizeof(u64) underflows");

[[nodiscard]]
static auto above(u64 sp) -> StackWindow {
    u64 const room = ~u64{0} - sp;
    return StackWindow{ .lo = sp, .hi = room < STACK_WINDOW ? ~u64{0} : sp + STACK_WINDOW };
}

if (address < window->lo || address > window->hi - sizeof(u64)) {
    return false;
}
if ((address % alignof(u64)) != 0) {
    return false;
}
```

`hi - sizeof(u64)` 不会下溢：窗口至少这么宽（static_assert 保证），而 `above()` 不回绕。

顺带补了对齐检查。x86 容忍非对齐加载，但这个库同样为 arm / riscv 构建，在那里同一个损坏
指针会在**中断处理程序内部**触发异常，而不是被干净地拒绝。

**参照**

无，本项目自有代码。同类规则见 `Linux access_ok()`：它同样避免构造 `addr + size`，
用 `size <= limit - addr` 的形式，理由一模一样。

**验证**

构建通过；`clang-tidy` 对本文件无新告警；prof 的宿主单测全过（sampler 6 项、format 4 项）。

**故意没做**

- 没有为这个守卫加回归测试。`kernel_stack_read` 在匿名命名空间里，从外部够不着，而端到端
  触发它需要真实的 FDE 索引让展开器走完一步。要可测就得把 `StackWindow` 提成一个有
  `contains()` 的具名类型放进 `prof/`——那是对的方向（守卫自己守住自己的不变量），
  但属于结构改动，单独记进待修。
