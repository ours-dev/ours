# panic 改动记录

## 断言的消息终于被格式化了，而且渲染好的文本不再被当成格式串重新解析（2026-09-09）

**文件**
- `system/public/ours/panic.hpp` — 拆出 `detail::render()`，新增 `panic_assertion()` 两个重载
- `system/public/ours/assert.hpp` — `ASSERT` 改成调 `panic_assertion()`，去掉 `if constexpr`
- `kernel/top/panic.cpp` — `vpanic(string_view)` 把消息当参数而不是格式串
- `kernel/phys/panic.cpp` — 同上

**症状**
带参数的断言一条都读不到。AP 起来之后立刻打出的是：

```
[4248802000 cpu1 error] format_error was thrown in -fno-exceptions mode with message "%s"
```

真正失败的那条断言是

```cpp
DEBUG_ASSERT(m_runtime.which_cpu == Cpu::id(), "Invoke {}() on mismatched CPU[{}]", __func__, ...)
```

——一个字都没露出来。要靠 lldb 挂 `std::__throw_format_error` 断点、`bt` 出来才知道
是谁在断言。

**原因**

**1. `ASSERT` 的消息从来没有被格式化过。** 宏原来展开成：

```cpp
ours::panic("Assertion failed on {} with message: {}", #condition __VA_OPT__(,) __VA_ARGS__);
```

格式串里有**两个** `{}`，而 `__VA_ARGS__` 是「消息 + 消息自己的参数」。
`std::format` 对多余的实参是宽容的——直接忽略。于是：

- 消息里的 `{}` 原样留在了输出里，因为它是被当作**第二个 `{}` 的取值**填进去的，
  填进去的东西不会再被解析一遍；
- 消息自己的参数（`__func__`、cpu 号）被静默丢弃。

也就是说，这套断言**只有不带参数的消息是对的**。`DEBUG_ASSERT(cpu < MAX_CPU,
"Invalid CPU Number")` 一直工作正常，掩盖了这件事。

**2. 渲染好的文本又被解析了一遍。** `panic()` 把结果格式化进 512 字节栈缓冲，
然后交给 `vpanic(std::string_view fmt)`，而后者是这么写的：

```cpp
log::vlog(logz4::get_global_logger(), logz4::Level::Error, fmt, std::make_format_args());
```

`fmt` ——已经渲染完的成品文本——被当成格式串，配零个实参。上面第 1 条留在文本里的
`{}` 于是变成了一个没有实参的替换字段，`std::format` 抛 `format_error`。
`-fno-exceptions` 下这会走到 libc++ 的 `__libcpp_verbose_abort`，而本项目的实现是
`ours::panic(format)`——**丢掉可变参数**，所以连 libc++ 想说的那句话也没了，只剩一个
没被替换的 `%s`。

三层叠在一起：断言不格式化 → 成品被二次解析 → 报错信息本身也被吃掉。
结果是一条断言失败在控制台上表现为一句和它毫无关系的话。

`kernel/phys/panic.cpp` 的注释甚至已经写明「消息由 `panic()` 预渲染好了」，
下一行却仍然把它当格式串传下去——这是同一个错在两个地方。

**改法**
```cpp
// panic.hpp
namespace detail {
    template <typename... Args>
    auto render(PanicSink &sink, std::string_view fmt, Args &&...args) -> void;
}

inline auto panic_assertion(std::string_view condition) -> void {
    panic("Assertion failed on {}", condition);
}

template <typename... Args>
auto panic_assertion(std::string_view condition, std::string_view fmt, Args &&...args) -> void {
    char buffer[512];
    auto sink = PanicSink{buffer, buffer + sizeof(buffer) - 1};
    detail::render(sink, "Assertion failed on {} with message: ", condition);
    detail::render(sink, fmt, static_cast<Args &&>(args)...);   // 消息是它自己的格式串
    vpanic(std::string_view(buffer, usize(sink.m_out - buffer)));
}
```
```cpp
// panic.cpp
auto vpanic(std::string_view message) -> void {
    ...
    log::vlog(logz4::get_global_logger(), logz4::Level::Error, std::string_view("{}"),
              std::make_format_args(message));   // 成品当实参，不当格式串
    ...
}
```

`ASSERT` 里那个 `if constexpr (__VA_OPT__(0 ==) 1)` 一并去掉了：非模板上下文里
`if constexpr` 的两个分支**都要能编译**，所以「无消息」那条展开必须写出一个它永远不会
调用的调用，而那行代码就这么悄悄过期了（改成 `panic_assertion` 之后它直接编译不过，
才把这件事暴露出来）。现在由重载决议按实参个数选，两条路径都是真的会被走到的。

被否掉的方案：给 `panic_assertion` 的 `fmt` 一个默认值再跟可变包。能编译，但把
「有消息」和「没消息」两种形态糊在一个签名里，读的人分不清哪条路是哪条。

**参照**
无，本项目自有设计。形状上对应 Linux 的 `__warn()` / `assertion_failed()`：条件和
消息是两段独立的文本，各自渲染后拼接，不互相当格式串。

**验证**
`SMP=4` 实测，同一条断言现在打出的是：

```
[4129816000 cpu1 error] Assertion failed on m_runtime.which_cpu == Cpu::id() \
    with message: Invoke reschedule_common() on mismatched CPU[1]
```

`SMP=1` 的引导行为不变（走到 userboot、两个示例线程照常打印）。
全树 grep 过没有「消息里带 `{}` 但不带参数」的断言，所以新的「消息即格式串」语义
不会把现存的哪一条打坏。

**故意没做**
- `__libcpp_verbose_abort(char const *format, ...)` 仍然把可变参数丢给
  `ours::panic(format)`。libc++ 传进来的是 printf 风格的格式串（`%s`），
  和这里的 `std::format` 语法不是一回事，要正确渲染得先有一个 vsnprintf。
  记在 `docs/repairs/260909/panic.md`。
