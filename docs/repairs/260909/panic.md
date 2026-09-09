# panic —— 待修项（2026-09-09）

## 1. `__libcpp_verbose_abort()` 把 libc++ 想说的话丢了

**严重程度**：中。它是"错误信息本身出错"的那一类，代价按小时算。

`kernel/lib/libc/libcxx.cpp:139`：

```cpp
void __libcpp_verbose_abort(char const *format, ...) noexcept {
    ours::panic(format);
}
```

libc++ 传进来的是 **printf 风格**的格式串加可变参数，例如

```
"format_error was thrown in -fno-exceptions mode with message \"%s\""
```

这里把可变参数整个丢掉，`%s` 原样打到控制台。今天的四核引导上就吃了这一发：
一条断言失败在控制台上表现为

```
[4248802000 cpu1 error] format_error was thrown in -fno-exceptions mode with message "%s"
```

真正的信息（"The argument index value is too large for the number of arguments supplied"）
和真正失败的那条断言都没了，只能靠 lldb 挂 `std::__throw_format_error` 断点、`bt`
才找出来。

**怎么修**：要正确渲染得先有一个 `vsnprintf`——注意**不能**改用 `std::format`，
libc++ 给的是 printf 语法，两套不通用。`system/lib/libc` 下有没有可用的实现要先查；
没有的话，退而求其次至少把格式串里的 `%s` 之前那一段打出来，比现在强。

`kernel/lib/ccrt/compiler/itanium.cpp:72`（phys 那一份）同样丢参数，
但它连格式串都不用，直接打 "libc++ invariant failed in physical boot"，
至少不会假装自己说了什么。
