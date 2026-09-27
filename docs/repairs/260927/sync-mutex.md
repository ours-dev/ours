# kernel/sync Mutex 待修项（2026-09-27）

## 1. `Mutex::assert_held()` 和 `Mutex::is_locked()` 只有声明，没有定义

**现象**：`kernel/include/ours/sync/mutex.hpp` 声明了

```cpp
auto is_locked() const -> bool;

[[nodiscard]]
TA_ASSERT() auto assert_held() const -> bool;
```

但 `kernel/kernel/sync/` 下没有它们的定义，全树也没有调用方。第一个调用它们的人会在链接时失败。
`260927/mm.md` 给创建映射加锁时，本想用 `get_lock().assert_held()` 在
`create_subaom_preamble()` 里做运行期检查，就是因为这个放弃了。

**另一个问题**：`assert_held()` 返回 `bool` 且标了 `[[nodiscard]]`，但 `TA_ASSERT` 的语义是
"调用返回后，锁一定被持有"。一个返回布尔值、要调用方自己判断的函数，和这个注解是矛盾的。
`SpinLock::assert_held()` 返回 `void`，内部直接 `ASSERT`（`kernel/kernel/sync/spin_lock.cpp`），
这才是 `TA_ASSERT` 对应的形状。

**怎么修**：
- `assert_held()` 改成返回 `void`，定义为 `ASSERT(holder() == task::Thread::Current::get())`；
  `holder()` 已经有了。
- `is_locked()` 定义为读状态字，看是否有持有者；或者如果确实没人需要它，就直接删掉声明。

**照谁改**：Zircon `Mutex::AssertHeld()`（`kernel/include/kernel/mutex.h`）是
`void`，标 `TA_ASSERT()`，内部 `DEBUG_ASSERT(IsHeld())`；`IsHeld()` 比较持有者和当前线程。
