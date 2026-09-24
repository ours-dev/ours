## 字节大小字面量 `_kb`…`_pb` 改为 `usize`；`_pb` 移 50 位（2026-09-23）

**文件**
- `kernel/include/ours/prelude.hpp` — `_kb`/`_mb`/`_gb`/`_tb`/`_pb` 返回 `usize`；`_pb` 由 `<< 40` 改为 `<< 50`；加两条 `static_assert`

**症状**
- 这组字面量返回 `isize`。`stack.hpp` 把 `KB(16)` 换成 `16_kb` 之后，`constexpr static auto DEFAULT_STACK_SIZE`
  被推导成有符号数，而它唯一的用处 `Stack::init(usize size = DEFAULT_STACK_SIZE)` 要的是 `usize`。
- `_pb` 移 40 位，和 `_tb` 一样：`1_pb` 其实是 1 TiB。今天没人用，所以没暴露。

**原因**
大小不会是负数，接收大小的接口（`Stack::init()`、`kmalloc()`、映射长度）也都收 `usize`；返回有符号数只会让
`auto` 声明处的类型和用处不一致。`_pb` 是复制 `_tb` 那一行时没改位数。

**改法**
改前：

```cpp
constexpr auto operator""_kb(unsigned long long v) -> isize {
    return isize(v) << 10;
}
...
constexpr auto operator""_pb(unsigned long long v) -> isize {
    return isize(v) << 40;
}
```

改后：

```cpp
constexpr auto operator""_kb(unsigned long long v) -> usize {
    return usize(v) << 10;
}
...
constexpr auto operator""_pb(unsigned long long v) -> usize {
    return usize(v) << 50;
}

static_assert(16_kb == 16384);
static_assert(1_pb == 1024 * 1_tb);
```

**参照**
无，本项目自有的字面量。

**验证**
全树只有 `stack.hpp` 一处在用；构建通过（prelude 会被强制包含进内核、用户程序与宿主构建），两个镜像启动正常。
两条 `static_assert` 在每次构建时检查数值。

**故意没做**
没有顺带把 `system/public/ours/macro_abi.hpp` 里的 `KB()`/`MB()` 宏清掉：它们还有大量使用者，是另一件事。
