# prof 改动记录

## FDE 索引改用 `new`，不再手写页分配（2026-09-08）

**文件**
- `kernel/commands/prof.cpp` — `get_frames_n` 换成 `new (GAF_KERNEL) Entry[count]`

**症状**
无回归，是把一处绕路收回来。

**原因**
这块表的注释本身就是一份"为什么绕路"的记录：

```
// ... which is past the largest kmalloc size class (4 KiB), and `ktl::kmalloc()`
// answers an oversized request with nullptr rather than falling back to
// pages. That rules out `ktl::Vec` here: its allocator constructs the
// elements at whatever `operator new[]` returned, so a nullptr becomes a
// page fault in `reserve()`, which is exactly how this hook used to hang
// every boot.
```

两个理由现在都不成立了：`operator new` 会把超过 size class 阶梯的请求路由给 vmalloc，
而 placement 形式加了 `noexcept`——失败是一个空指针，不是在空指针上跑构造函数
（见 `docs/changes/260908/heap.md`）。

**改法**

```cpp
// 改前
usize const bytes = count * sizeof(Entry);
usize const nr_frames = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
auto *entries = mm::get_frames_n<Entry>(mm::GAF_KERNEL, nr_frames);

// 改后
auto *entries = new (mm::GAF_KERNEL) Entry[count];
```

顺带修掉一处浪费：`get_frames_n` 会把页数**向上取整到 2 的幂**（`pmm.hpp:129`），
所以 ~25 KiB 实际拿的是 32 KiB，多出来的一页多是白扔的；vmalloc 只取整到页。

**仍然是裸数组而不是 `ktl::Vec`**：索引分两趟建，第一趟只计数，所以它是按最终精确尺寸
一次分配、永不增长的。也从不释放——它活得和它描述的那段代码一样久。

**参照**
无对应物；Linux 的 `.eh_frame` 索引不在内核里。

**验证**
QEMU 实测 `kernel.dev.dbg_1.obi`：

```
prof: indexed 860 unwind frames (31548 bytes of .eh_frame)
```

860 × 32 B ≈ 27 KiB，超过 4 KiB 阶梯，所以这次分配走的正是 vmalloc。
`vmalloc: self test passed`、userboot 照常，无回归。
