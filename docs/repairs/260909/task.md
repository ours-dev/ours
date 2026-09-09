# task —— 待修项（2026-09-09）

## 1. `ktl::Name` 没有 `format()`，三处调用方各自摆弄裸缓冲区

**严重程度**：低（今天已经把三处都修对了，见 `docs/changes/260909/task.md`），
但同一个错在三个文件里独立犯了三遍，说明缺的是一个 idiom 而不是三处笔误。

现在每个调用方都得写：

```cpp
char name[24];
auto const end = ustl::fmt::format_to_n(name, sizeof(name) - 1, "Idle/{}", cpu).out;
*end = '\0';
Thread::Current::set_name(name);
```

三行里有两行是仪式。`ktl::Name` 自己知道自己的容量，也知道自己必须以 NUL 结尾，
让它长出

```cpp
template <typename... Args>
auto format(std::format_string<Args...> fmt, Args &&...args) -> void;
```

之后调用方就只剩一行，而且**不可能**再写出没有终止符的名字。
`Thread::Current::set_name()` 也该有一个对应的重载。

**注意**：`ktl::Name` 在 `kernel/lib/ktl`，加 `<format>` 依赖之前先确认它的使用者
（`kernel/object/.../resource_dispatcher.hpp` 等）都有 ustl/libc++ 的 include path。
