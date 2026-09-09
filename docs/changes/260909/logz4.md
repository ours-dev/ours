# logz4 改动记录

## `log::info("{}", char_buf)` 打出的是空——字符数组要先退化成指针（2026-09-09）

**文件**
- `system/lib/ustl/include/ustl/fmt/make_format_args.hpp` — 新增 `decay_format_arg()`
- `system/lib/logz4/include/logz4/log.hpp` — `log()` 把每个实参过一遍它
- `system/public/ours/panic.hpp` — 只改注释，说明为什么这里不能复用 ustl 那份

**症状**
排查 DPC 线程名的时候加了一行

```cpp
log::info("XXX dpc name |{}|", name);   // char name[24]
```

打出来是

```
[3700221000 cpu0 info] XXX dpc name ||
```

而 `name` 里躺着的是完好的 `"Dpc/0/Low"`。换成
`static_cast<char const *>(name)` 就正常了。

**原因**
这个 libc++ 里，`std::make_format_args` 收到一个 `char[24]` 时，存进 argument store
的是**数组类型**而不是 `const char *`，格式化阶段对它什么也不输出——不报错，不诊断，
就是一个渲染成空的 `{}`。指针和 `string_view` 都正常。

这一点仓库里其实早就吃过一次亏：`system/public/ours/panic.hpp` 的
`detail::as_format_arg()` 就是为它写的，注释里记着「断言打出 `Assertion failed on `
然后就没了，刚刚失败的那个条件不见了」。`logz4::log()` 没有做同样的处理。

一个会**静默**吞掉实参的日志设施格外糟糕：你加那一行日志正是因为不知道发生了什么，
而它给你的是一个错误的答案。今天这一行几乎让人以为线程名真的是空的——而真正的 bug
在别处（名字没有 NUL 结尾，见 `task.md`）。

**改法**
把那个 helper 提到 `ustl::fmt::decay_format_arg()`，`logz4::log()` 用它：

```cpp
template <typename... Args>
inline auto log(Level level, ustl::views::StringView fmt, Args &&...args) -> void {
    return [&](auto &&...prepared) {
        vlog_dispatch(get_global_logger(), level, fmt, ustl::fmt::make_format_args(prepared...));
    }(ustl::fmt::decay_format_arg(ustl::forward<Args>(args))...);
}
```

外面这层 lambda 不是装饰：`decay_format_arg()` 可能返回一个 prvalue `string_view`，
而 `make_format_args()` 要求左值，绑到 lambda 的形参上才能活过整个格式化调用。
`panic()` 里是同一套写法。

**被否掉的方案**：让 `panic.hpp` 也改用 `ustl::fmt::decay_format_arg()`，一份实现两处用。
编译不过——`panic.hpp` 在 `system/public` 下，被一批**根本没有把 ustl 放进 include path**
的翻译单元包含（`kernel/lib/ccrt/compiler/itanium.cpp`、
`kernel/lib/libc/libcxx_format_glue.cpp`）。两份实现是这个分层的代价，
在两边的注释里都写清楚了。

**参照**
无，本项目自有设计。标准里 `std::make_format_args` 对数组实参的行为本就是
implementation-defined 的灰区，fmtlib 会把它退化成 `string_view`，这个 libc++ 不会。

**验证**
同一行日志，改前 `|| || ||`，改后：

```
[4030306000 cpu0 info] XXX dpc name |Dpc/0/Low|
[4031180000 cpu0 info] XXX dpc name |Dpc/0/Normal|
[4031486000 cpu0 info] XXX dpc name |Dpc/0/High|
```

四核引导照常（cpu0 8589 行 / cpu1 653 / cpu2 92554 / cpu3 92542，无断言失败）。

**故意没做**
- 没有去查这个 libc++ 到底为什么不退化数组，也没有考虑升级工具链绕开它。
- `ustl::fmt::format_to()` 的实参没有走同样的退化——它是 `std::format_to` 的直接别名，
  格式串是编译期检查的，数组实参会在编译期就被拒绝，不会静默。
