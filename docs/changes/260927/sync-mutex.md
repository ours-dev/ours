## Mutex::assert_held() 和 is_locked() 补上定义，assert_held() 改为返回 void（2026-09-27）

修掉 `docs/repairs/260927/sync-mutex.md` 第 1 条。那个文件只有这一条，修完后删除。

**文件**
- `kernel/include/ours/sync/mutex.hpp`：`assert_held()` 改为返回 `void`、去掉 `[[nodiscard]]`；
  `is_locked()` 加 `[[nodiscard]]`；两者补注释
- `kernel/kernel/sync/mutex.cpp`：补上两者的定义
- `docs/repairs/260927/sync-mutex.md`：删除

**症状**
两者都只有声明、没有定义，全树也没有调用方。第一个调用的人会链接失败。
给地址空间锁补线程安全注解时正好需要 `assert_held()`（见 `260927/mm.md`），因此被卡住。

**原因**
除了缺定义，`assert_held()` 的形状也是错的：

```cpp
[[nodiscard]]
TA_ASSERT() auto assert_held() const -> bool;
```

`TA_ASSERT` 告诉分析器"这个调用返回后，锁一定被当前线程持有"。可返回 `bool` 又加 `[[nodiscard]]`，
意思却是"我告诉你持没持有，你自己判断"。调用方可以拿到 `false` 然后照样往下走，而分析器已经
把锁当成持有了。两个说法互相矛盾。`SpinLock::assert_held()` 返回 `void`、内部直接断言，
那才是与 `TA_ASSERT` 相符的写法。

**改法**

之后：

```cpp
[[nodiscard]]
auto is_locked() const -> bool;

TA_ASSERT() auto assert_held() const -> void;

// mutex.cpp
auto Mutex::is_locked() const -> bool {
    return holder() != nullptr;
}

auto Mutex::assert_held() const -> void {
    DEBUG_ASSERT(holder() == Thread::Current::get(), "the calling thread does not hold this mutex");
}
```

运行期检查用 `DEBUG_ASSERT`，与 Zircon 相同：只在调试构建里检查，但对分析器的声明在所有构建里都成立。
`holder()` 本来就有，它从状态字里去掉低位标志，取出持有者。

**参照**
Zircon `Mutex::AssertHeld()`（`kernel/include/kernel/mutex.h`）：返回 `void`，标 `TA_ASSERT()`，
内部 `DEBUG_ASSERT(IsHeld())`；`IsHeld()` 比较 `holder()` 与 `Thread::Current::Get()`。

**验证**
- `kernel.dev.obi` 和 `kernel.dev.dbg_1.obi` 均构建通过；clang-tidy 在改动行上无告警。
- QEMU 实测：两个镜像的 `usercopy.test` 都是 18/18，无异常。这次提交里还没有调用方，运行期断言
  由下一步 mm 的注解工作用上。

**故意没做**
没有顺手做 `sync::CriticalMutex`（它目前是个空壳）。那是 sync 的另一件事。
